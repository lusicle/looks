#include "media/player.h"

#include <cmath>

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "media/sample_clock.h"
#include "util/log.h"

namespace looks::media {

namespace {
constexpr uint32_t kMonitorRate = Player::kClockRate;
constexpr uint32_t kMonitorChannels = Player::kChannels;
}  // namespace

struct Player::Impl {
    // The cursor counts samples at the monitor rate. The audio callback
    // advances it; all other threads only read it.
    std::atomic<uint64_t> cursor{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> looping{true};
    std::atomic<uint32_t> frames{0};
    std::atomic<uint32_t> trim_in{0};
    std::atomic<uint32_t> trim_out{0};
    std::atomic<uint32_t> loop_in{0};
    std::atomic<uint32_t> loop_out{0};
    std::atomic<int64_t> audio_offset_samples{0};
    std::atomic<float> gain{1.0f};
    std::atomic<double> fps{30.0};
    uint32_t clock_rate = kMonitorRate;

    // The UI thread publishes the mix; the callback pulls it.
    std::atomic<std::shared_ptr<const MixState>> mix{nullptr};
    // The callback must never drop the last mix reference; the UI thread
    // retires two generations to prevent it.
    std::mutex retire_mutex;
    std::shared_ptr<const MixState> retired[2];
    int retire_slot = 0;
    std::vector<float> mix_scratch;
    std::vector<int16_t> mix_out;

    std::mutex tick_mutex;
    std::chrono::steady_clock::time_point last_tick;
    bool use_audio_clock = false;

    // Open the device on its own thread: WASAPI makes that thread MTA,
    // and the UI thread must stay STA for file dialogs.
    ma_device device{};
    bool device_started = false;
    std::thread device_thread;
    std::mutex device_mutex;
    std::condition_variable device_cv;
    bool device_ready = false;
    bool device_quit = false;

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(device_mutex);
            device_quit = true;
        }
        device_cv.notify_all();
        if (device_thread.joinable()) device_thread.join();
    }

    // Init and uninit must run on one thread; WASAPI is apartment-bound.
    void device_main() {
        ma_device_config config = device_config();
        const bool ok =
            ma_device_init(nullptr, &config, &device) == MA_SUCCESS &&
            ma_device_start(&device) == MA_SUCCESS;
        {
            std::lock_guard<std::mutex> lock(device_mutex);
            device_started = ok;
            use_audio_clock = ok;
            device_ready = true;
        }
        device_cv.notify_all();
        if (!ok) {
            log_warn("transport: no audio device — wall clock");
            return;
        }
        std::unique_lock<std::mutex> lock(device_mutex);
        device_cv.wait(lock, [this] { return device_quit; });
        lock.unlock();
        ma_device_uninit(&device);
    }

    ma_device_config device_config() {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = kMonitorChannels;
        config.sampleRate = kMonitorRate;
        config.dataCallback = &Impl::audio_callback;
        config.pUserData = this;
        return config;
    }

    // configure() keeps fps above 0, so do not check it here.
    // All frame and cursor conversions must use the sample_clock.h rule.
    uint64_t frame_to_cursor(uint32_t frame) const {
        return static_cast<uint64_t>(frame_to_sample(
            frame, fps.load(std::memory_order_relaxed), clock_rate));
    }

    uint32_t cursor_to_frame(uint64_t c) const {
        // The 1e-6 absorbs ulps below an exact frame boundary.
        const double frames_f =
            static_cast<double>(c) * fps.load(std::memory_order_relaxed) /
            clock_rate;
        const uint32_t frame = static_cast<uint32_t>(frames_f + 1e-6);
        const uint32_t out = trim_out.load();
        return frame >= out ? (out ? out - 1 : 0) : frame;
    }

    void loop_bounds(uint32_t& in_f, uint32_t& out_f) const {
        in_f = trim_in.load();
        out_f = trim_out.load();
        const uint32_t li = loop_in.load(), lo = loop_out.load();
        if (lo > li) {
            const uint32_t ci = in_f > li ? in_f : li;
            const uint32_t co = out_f < lo ? out_f : lo;
            if (co > ci) {
                in_f = ci;
                out_f = co;
            }
        }
    }

    uint64_t advance_cursor(uint64_t current, uint64_t delta) {
        uint32_t in_f, out_f;
        loop_bounds(in_f, out_f);
        const uint64_t in_c = frame_to_cursor(in_f);
        const uint64_t out_c = frame_to_cursor(out_f);
        const uint64_t trim_in_c = frame_to_cursor(trim_in.load());
        uint64_t next = current + delta;
        if (next >= out_c) {
            if (looping.load()) {
                // A playhead before the loop region plays into it, then wraps.
                const uint64_t base = next >= in_c ? in_c : trim_in_c;
                const uint64_t span = out_c > base ? out_c - base : 1;
                next = base + (next - base) % span;
            } else {
                const uint64_t end_c = frame_to_cursor(trim_out.load());
                next = end_c > 0 ? end_c - 1 : 0;
                playing.store(false);
            }
        }
        if (next < trim_in_c) next = trim_in_c;
        return next;
    }

    static void audio_callback(ma_device* dev, void* output,
                               const void* /*input*/, ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        int16_t* out = static_cast<int16_t*>(output);
        const uint32_t ch = kMonitorChannels;
        std::memset(out, 0, static_cast<size_t>(frame_count) * ch * 2);
        if (!self->playing.load(std::memory_order_relaxed)) return;

        const uint64_t begin = self->cursor.load(std::memory_order_relaxed);
        // Each run must stay contiguous in timeline samples.
        uint64_t c = begin;
        const float gain = self->gain.load(std::memory_order_relaxed);
        const int64_t offset =
            self->audio_offset_samples.load(std::memory_order_relaxed);
        std::shared_ptr<const MixState> mix =
            self->mix.load(std::memory_order_acquire);

        ma_uint32 done = 0;
        while (done < frame_count) {
            const uint64_t run_start = c;
            ma_uint32 run = 0;
            while (done + run < frame_count) {
                const uint64_t next = self->advance_cursor(c, 1);
                ++run;
                if (next != c + 1) {
                    c = next;
                    break;
                }
                c = next;
                if (!self->playing.load(std::memory_order_relaxed)) break;
            }
            if (run == 0) break;
            // A mix with another channel count reads as garbage; keep silence.
            if (mix && mix->channels == ch && gain > 0.0f) {
                if (self->mix_out.size() < static_cast<size_t>(run) * ch)
                    self->mix_out.resize(static_cast<size_t>(run) * ch);
                render_mix(*mix,
                           static_cast<int64_t>(run_start) - offset,
                           self->mix_out.data(), run, self->mix_scratch);
                int16_t* dst = out + static_cast<size_t>(done) * ch;
                for (size_t i = 0; i < static_cast<size_t>(run) * ch; ++i)
                    dst[i] = static_cast<int16_t>(
                        std::clamp(static_cast<float>(self->mix_out[i]) * gain,
                                   -32768.0f, 32767.0f));
            }
            done += run;
            if (!self->playing.load(std::memory_order_relaxed)) break;
        }
        self->cursor.store(c, std::memory_order_relaxed);
    }

    void tick_synthetic_clock() {
        if (use_audio_clock) return;
        std::lock_guard<std::mutex> lock(tick_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (playing.load()) {
            const double dt =
                std::chrono::duration<double>(now - last_tick).count();
            const uint64_t delta = static_cast<uint64_t>(dt * clock_rate);
            if (delta > 0) {
                const uint64_t c = cursor.load();
                cursor.store(advance_cursor(c, delta));
                last_tick = now;
            }
        } else {
            last_tick = now;
        }
    }
};

Player::Player() : impl_(new Impl) {
    Impl& p = *impl_;
    p.last_tick = std::chrono::steady_clock::now();
    p.device_thread = std::thread([&p] { p.device_main(); });
    std::unique_lock<std::mutex> lock(p.device_mutex);
    p.device_cv.wait(lock, [&p] { return p.device_ready; });
}

Player::~Player() = default;

void Player::configure(double fps, uint32_t frames) {
    Impl& p = *impl_;
    const double rate = fps > 0.0 ? fps : 30.0;
    if (p.fps.load() == rate && p.frames.load() == frames) return;
    p.fps.store(rate);
    p.frames.store(frames);
    uint32_t in_f = p.trim_in.load(), out_f = p.trim_out.load();
    if (out_f == 0 || out_f > frames) out_f = frames;
    if (in_f >= out_f) in_f = out_f ? out_f - 1 : 0;
    p.trim_in.store(in_f);
    p.trim_out.store(out_f);
    p.cursor.store(p.advance_cursor(p.cursor.load(), 0));
}

bool Player::active() const { return impl_->frames.load() > 0; }

void Player::set_mix(std::shared_ptr<const MixState> mix) {
    Impl& p = *impl_;
    std::shared_ptr<const MixState> previous =
        p.mix.exchange(std::move(mix), std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(p.retire_mutex);
    p.retired[p.retire_slot] = std::move(previous);
    p.retire_slot = (p.retire_slot + 1) % 2;
}

void Player::play() {
    {
        std::lock_guard<std::mutex> lock(impl_->tick_mutex);
        impl_->last_tick = std::chrono::steady_clock::now();
    }
    impl_->playing.store(true);
}

void Player::tick() { impl_->tick_synthetic_clock(); }

void Player::pause() { impl_->playing.store(false); }
bool Player::playing() const { return impl_->playing.load(); }
void Player::set_looping(bool loop) { impl_->looping.store(loop); }
void Player::set_gain(float gain) {
    impl_->gain.store(std::clamp(gain, 0.0f, 2.0f));
}

uint32_t Player::frame_count() const { return impl_->frames.load(); }
double Player::fps() const { return impl_->fps.load(); }

double Player::duration_seconds() const {
    const double f = impl_->fps.load();
    return f > 0.0 ? impl_->frames.load() / f : 0.0;
}

void Player::set_trim(uint32_t in_frame, uint32_t out_frame) {
    Impl& p = *impl_;
    const uint32_t total = p.frames.load();
    if (out_frame > total) out_frame = total;
    if (in_frame >= out_frame) in_frame = out_frame ? out_frame - 1 : 0;
    p.trim_in.store(in_frame);
    p.trim_out.store(out_frame);
    p.cursor.store(p.advance_cursor(p.cursor.load(), 0));
}

uint32_t Player::trim_in() const { return impl_->trim_in.load(); }
uint32_t Player::trim_out() const { return impl_->trim_out.load(); }

void Player::set_loop_region(uint32_t in_frame, uint32_t out_frame) {
    impl_->loop_in.store(in_frame);
    impl_->loop_out.store(out_frame);
}

bool Player::looping() const { return impl_->looping.load(); }

void Player::loop_bounds(uint32_t* in_frame, uint32_t* out_frame) const {
    uint32_t i = 0, o = 0;
    impl_->loop_bounds(i, o);
    if (in_frame) *in_frame = i;
    if (out_frame) *out_frame = o;
}

void Player::set_audio_offset(double seconds) {
    impl_->audio_offset_samples.store(
        seconds_to_samples(seconds, impl_->clock_rate));
}

void Player::seek_frame(uint32_t frame_index) {
    Impl& p = *impl_;
    const uint32_t in_f = p.trim_in.load();
    const uint32_t out_f = p.trim_out.load();
    if (frame_index < in_f) frame_index = in_f;
    if (frame_index >= out_f) frame_index = out_f ? out_f - 1 : 0;
    p.cursor.store(p.frame_to_cursor(frame_index));
    {
        std::lock_guard<std::mutex> lock(p.tick_mutex);
        p.last_tick = std::chrono::steady_clock::now();
    }
}

uint32_t Player::current_frame_index() const {
    return impl_->cursor_to_frame(impl_->cursor.load());
}

double Player::position_seconds() const {
    return static_cast<double>(impl_->cursor.load()) / impl_->clock_rate;
}

uint32_t Player::audio_channels() const { return kMonitorChannels; }
uint32_t Player::audio_sample_rate() const { return kMonitorRate; }

}  // namespace looks::media
