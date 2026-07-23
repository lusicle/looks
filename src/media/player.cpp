#include "media/player.h"

#include <miniaudio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "media/pcm.h"
#include "util/log.h"

namespace looks::media {

namespace {
constexpr uint32_t kLookahead = 8;   // decoded frames ahead of the playhead
}

struct Player::Impl {
    // ---- media
    codec::MezReader reader;          // owned by the decode worker after open
    uint32_t frames = 0;
    uint32_t timescale = 0;
    uint32_t frame_duration = 1;
    double frames_per_second = 0.0;

    std::vector<int16_t> pcm;         // interleaved, whole sidecar
    uint32_t channels = 0;
    uint32_t sample_rate = 0;

    // ---- timeline (master clock)
    // Position is a sample cursor at audio rate (or the synthetic rate for
    // silent clips). The audio callback advances it; everything else reads.
    std::atomic<uint64_t> cursor{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> looping{true};
    std::atomic<uint32_t> trim_in{0};
    std::atomic<uint32_t> trim_out{0};
    // Loop region (spec §9): confines looping inside the trim; 0/0 = the
    // whole trim loops.
    std::atomic<uint32_t> loop_in{0};
    std::atomic<uint32_t> loop_out{0};
    // Audio nudge (spec §7): positive delays audio against video.
    std::atomic<int64_t> audio_offset_samples{0};
    uint32_t clock_rate = 48000;      // cursor units per second

    // Silent-clip fallback clock. `tick_mutex` guards last_tick: both the
    // render worker and the UI thread tick it.
    std::mutex tick_mutex;
    std::chrono::steady_clock::time_point last_tick;
    bool use_audio_clock = false;

    // ---- audio device
    ma_device device{};
    bool device_started = false;

    // ---- decode ring
    struct RingEntry {
        uint32_t index;
        std::shared_ptr<const codec::DecodedFrame> frame;
    };
    std::mutex ring_mutex;
    std::deque<RingEntry> ring;
    std::condition_variable ring_cv;
    std::thread worker;
    std::atomic<bool> quit{false};

    ~Impl() { stop(); }

    void stop() {
        quit.store(true);
        ring_cv.notify_all();
        if (worker.joinable()) worker.join();
        if (device_started) {
            ma_device_uninit(&device);
            device_started = false;
        }
    }

    uint64_t frame_to_cursor(uint32_t frame) const {
        return static_cast<uint64_t>(
            static_cast<double>(frame) / frames_per_second * clock_rate + 0.5);
    }

    uint32_t cursor_to_frame(uint64_t c) const {
        const double seconds = static_cast<double>(c) / clock_rate;
        const uint32_t frame = static_cast<uint32_t>(seconds * frames_per_second);
        const uint32_t out = trim_out.load();
        return frame >= out ? (out ? out - 1 : 0) : frame;
    }

    // Loop bounds in frames: the loop region clamped inside the trim when
    // set, else the trim itself.
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

    // Wraps/clamps the cursor into the trim (or loop) region. Called by
    // the clock advancing side.
    uint64_t advance_cursor(uint64_t current, uint64_t delta) {
        uint32_t in_f, out_f;
        loop_bounds(in_f, out_f);
        const uint64_t in_c = frame_to_cursor(in_f);
        const uint64_t out_c = frame_to_cursor(out_f);
        const uint64_t trim_in_c = frame_to_cursor(trim_in.load());
        uint64_t next = current + delta;
        if (next >= out_c) {
            if (looping.load()) {
                // A playhead before the loop region plays into it, then
                // wraps inside it.
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

    // ---- audio callback: master clock + PCM feed
    static void audio_callback(ma_device* dev, void* output,
                               const void* /*input*/, ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        int16_t* out = static_cast<int16_t*>(output);
        const uint32_t ch = self->channels;
        std::memset(out, 0, static_cast<size_t>(frame_count) * ch * 2);
        if (!self->playing.load(std::memory_order_relaxed)) return;

        uint64_t c = self->cursor.load(std::memory_order_relaxed);
        const uint64_t total_pcm = self->pcm.size() / ch;
        const int64_t offset =
            self->audio_offset_samples.load(std::memory_order_relaxed);
        for (ma_uint32 i = 0; i < frame_count; ++i) {
            // Audio nudge: at cursor c the monitored sample is c - offset
            // (positive offset = audio later); out of range = silence.
            const int64_t s =
                static_cast<int64_t>(c) - offset;
            if (s >= 0 && s < static_cast<int64_t>(total_pcm)) {
                const int16_t* src =
                    self->pcm.data() + static_cast<uint64_t>(s) * ch;
                for (uint32_t k = 0; k < ch; ++k) out[i * ch + k] = src[k];
            }
            c = self->advance_cursor(c, 1);
            if (!self->playing.load(std::memory_order_relaxed)) break;
        }
        self->cursor.store(c, std::memory_order_relaxed);
        self->ring_cv.notify_one();
    }

    // Silent clips: step the cursor from wall time (called from readers).
    void tick_synthetic_clock() {
        if (use_audio_clock) return;
        std::lock_guard<std::mutex> lock(tick_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (playing.load()) {
            const double dt =
                std::chrono::duration<double>(now - last_tick).count();
            const uint64_t delta =
                static_cast<uint64_t>(dt * clock_rate);
            if (delta > 0) {
                uint64_t c = cursor.load();
                cursor.store(advance_cursor(c, delta));
                last_tick = now;
                ring_cv.notify_one();
            }
        } else {
            last_tick = now;
        }
    }

    // ---- decode worker
    void worker_main() {
        codec::DecodedFrame scratch;
        while (!quit.load()) {
            uint32_t target = cursor_to_frame(cursor.load());
            uint32_t missing = UINT32_MAX;
            {
                std::unique_lock<std::mutex> lock(ring_mutex);
                // Evict behind the playhead (keep the current frame).
                while (!ring.empty() && ring.front().index < target &&
                       !(ring.front().index <= target &&
                         ring.size() == 1))
                    ring.pop_front();

                // First missing index in [target, target+lookahead), with
                // loop wrap inside the trim (or loop) region.
                uint32_t in_f, out_f;
                loop_bounds(in_f, out_f);
                const uint32_t span = out_f > in_f ? out_f - in_f : 1;
                for (uint32_t k = 0; k < kLookahead; ++k) {
                    uint32_t idx = target + k;
                    if (idx >= out_f)
                        idx = looping.load() ? in_f + (idx - in_f) % span
                                             : out_f - 1;
                    bool present = false;
                    for (const RingEntry& e : ring)
                        if (e.index == idx) { present = true; break; }
                    if (!present) { missing = idx; break; }
                }
                if (missing == UINT32_MAX) {
                    ring_cv.wait_for(lock, std::chrono::milliseconds(5));
                    continue;
                }
            }

            if (!reader.decode(missing, scratch)) {
                log_warn("player: decode failed for frame %u", missing);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            auto frame = std::make_shared<codec::DecodedFrame>(std::move(scratch));
            scratch = codec::DecodedFrame{};
            {
                std::lock_guard<std::mutex> lock(ring_mutex);
                ring.push_back({missing, std::move(frame)});
            }
        }
    }
};

Player::Player() : impl_(new Impl) {}
Player::~Player() = default;

bool Player::open(const std::filesystem::path& mez_path,
                  const std::filesystem::path& pcm_path, std::string* error) {
    close();
    Impl& p = *impl_;

    if (!p.reader.open(mez_path, error)) return false;
    p.frames = p.reader.frame_count();
    p.timescale = p.reader.timescale();
    p.frame_duration = p.reader.frame_duration();
    p.frames_per_second = p.reader.fps() > 0.0 ? p.reader.fps() : 30.0;
    p.trim_in.store(0);
    p.trim_out.store(p.frames);

    // Audio sidecar -> RAM.
    p.use_audio_clock = false;
    if (!pcm_path.empty()) {
        PcmReader pcm_reader;
        std::string pcm_error;
        if (pcm_reader.open(pcm_path, &pcm_error)) {
            p.channels = pcm_reader.channels();
            p.sample_rate = pcm_reader.sample_rate();
            p.clock_rate = p.sample_rate;
            const uint64_t frames = pcm_reader.frame_count();
            p.pcm.resize(static_cast<size_t>(frames) * p.channels);
            pcm_reader.read(0, p.pcm.data(), static_cast<size_t>(frames));
            p.use_audio_clock = true;
        } else {
            log_warn("player: pcm sidecar unreadable (%s) — silent playback",
                     pcm_error.c_str());
        }
    }
    if (!p.use_audio_clock) {
        p.channels = 2;
        p.sample_rate = 48000;
        p.clock_rate = 48000;
    }

    // Audio device (only when there is audio to play; silent clips use the
    // synthetic clock and no device).
    if (p.use_audio_clock) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = p.channels;
        config.sampleRate = p.sample_rate;
        config.dataCallback = &Impl::audio_callback;
        config.pUserData = &p;
        if (ma_device_init(nullptr, &config, &p.device) != MA_SUCCESS) {
            log_warn("player: audio device init failed — silent playback");
            p.use_audio_clock = false;
        } else {
            if (ma_device_start(&p.device) != MA_SUCCESS) {
                ma_device_uninit(&p.device);
                log_warn("player: audio device start failed — silent playback");
                p.use_audio_clock = false;
            } else {
                p.device_started = true;
            }
        }
    }

    p.cursor.store(0);
    p.playing.store(false);
    p.quit.store(false);
    p.last_tick = std::chrono::steady_clock::now();
    p.worker = std::thread([impl = impl_.get()] { impl->worker_main(); });
    return true;
}

void Player::close() {
    impl_->stop();
    impl_->reader.close();
    impl_->ring.clear();
    impl_->pcm.clear();
    impl_->frames = 0;
    impl_->quit.store(false);
}

bool Player::is_open() const { return impl_->frames > 0; }

void Player::play() {
    {
        std::lock_guard<std::mutex> lock(impl_->tick_mutex);
        impl_->last_tick = std::chrono::steady_clock::now();
    }
    impl_->playing.store(true);
    impl_->ring_cv.notify_one();
}

void Player::tick() { impl_->tick_synthetic_clock(); }

void Player::pause() { impl_->playing.store(false); }
bool Player::playing() const { return impl_->playing.load(); }
void Player::set_looping(bool loop) { impl_->looping.store(loop); }

uint32_t Player::frame_count() const { return impl_->frames; }
double Player::fps() const { return impl_->frames_per_second; }

double Player::duration_seconds() const {
    return impl_->frames_per_second > 0.0
        ? impl_->frames / impl_->frames_per_second : 0.0;
}

void Player::set_trim(uint32_t in_frame, uint32_t out_frame) {
    Impl& p = *impl_;
    if (out_frame > p.frames) out_frame = p.frames;
    if (in_frame >= out_frame) in_frame = out_frame ? out_frame - 1 : 0;
    p.trim_in.store(in_frame);
    p.trim_out.store(out_frame);
    // Snap the cursor into the region.
    const uint64_t c = p.cursor.load();
    p.cursor.store(p.advance_cursor(c, 0));
    p.ring_cv.notify_one();
}

uint32_t Player::trim_in() const { return impl_->trim_in.load(); }
uint32_t Player::trim_out() const { return impl_->trim_out.load(); }

void Player::set_loop_region(uint32_t in_frame, uint32_t out_frame) {
    impl_->loop_in.store(in_frame);
    impl_->loop_out.store(out_frame);
}

void Player::set_audio_offset(double seconds) {
    impl_->audio_offset_samples.store(static_cast<int64_t>(
        seconds * impl_->clock_rate + (seconds >= 0.0 ? 0.5 : -0.5)));
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
    p.ring_cv.notify_one();
}

void Player::seek_seconds(double seconds) {
    seek_frame(static_cast<uint32_t>(
        seconds < 0.0 ? 0.0 : seconds * impl_->frames_per_second));
}

uint32_t Player::current_frame_index() const {
    return impl_->cursor_to_frame(impl_->cursor.load());
}

double Player::position_seconds() const {
    return static_cast<double>(impl_->cursor.load()) / impl_->clock_rate;
}

std::shared_ptr<const codec::DecodedFrame> Player::current_frame() {
    Impl& p = *impl_;
    p.tick_synthetic_clock();
    const uint32_t target = p.cursor_to_frame(p.cursor.load());
    std::lock_guard<std::mutex> lock(p.ring_mutex);
    // Exact frame, else the newest one at-or-before target, else anything.
    std::shared_ptr<const codec::DecodedFrame> best;
    uint32_t best_index = 0;
    bool found_below = false;
    for (const Impl::RingEntry& e : p.ring) {
        if (e.index == target) return e.frame;
        if (e.index < target && (!found_below || e.index > best_index)) {
            best = e.frame;
            best_index = e.index;
            found_below = true;
        }
    }
    if (!best && !p.ring.empty()) best = p.ring.front().frame;
    return best;
}

uint32_t Player::audio_channels() const { return impl_->channels; }
uint32_t Player::audio_sample_rate() const { return impl_->sample_rate; }

}  // namespace looks::media
