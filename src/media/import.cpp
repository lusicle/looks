#include "media/import.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "codec/mez.h"
#include "media/bmff.h"
#include "media/mp3.h"
#include "media/pcm.h"
#include "media/wav.h"
#include "mod/analysis.h"
#include "util/image.h"
#include "platform/win/mf_codec.h"
#include "platform/win/wic_image.h"
#include "util/log.h"

namespace looks::media {

namespace {

// NV12 -> I420: split interleaved UV. Output planes are tightly packed.
struct I420Frame {
    std::vector<uint8_t> y, u, v;
    uint32_t width = 0, height = 0;

    codec::FrameView view() const {
        const uint32_t cw = (width + 1) / 2;
        return {{y.data(), width}, {u.data(), cw}, {v.data(), cw},
                width, height};
    }
};

void nv12_to_i420(const platform::VideoFrameNV12& src, I420Frame& dst) {
    const uint32_t w = src.width;
    const uint32_t h = src.height;
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    dst.width = w;
    dst.height = h;
    dst.y.assign(src.data.begin(),
                 src.data.begin() + static_cast<size_t>(w) * h);
    dst.u.resize(static_cast<size_t>(cw) * ch);
    dst.v.resize(static_cast<size_t>(cw) * ch);
    const uint8_t* uv = src.data.data() + static_cast<size_t>(w) * h;
    for (uint32_t row = 0; row < ch; ++row) {
        const uint8_t* s = uv + static_cast<size_t>(row) * w;
        uint8_t* du = dst.u.data() + static_cast<size_t>(row) * cw;
        uint8_t* dv = dst.v.data() + static_cast<size_t>(row) * cw;
        for (uint32_t x = 0; x < cw; ++x) {
            du[x] = s[x * 2];
            dv[x] = s[x * 2 + 1];
        }
    }
}

// Half-res proxy downsample: 2x2 box average per plane, even
// output dims (the codec paths like them and so does NV12 export).
void downsample_half(const I420Frame& src, I420Frame& dst) {
    const uint32_t dw = std::max(2u, (src.width / 2) & ~1u);
    const uint32_t dh = std::max(2u, (src.height / 2) & ~1u);
    dst.width = dw;
    dst.height = dh;
    const uint32_t scw = (src.width + 1) / 2;
    const uint32_t sch = (src.height + 1) / 2;
    const uint32_t dcw = (dw + 1) / 2;
    const uint32_t dch = (dh + 1) / 2;
    auto box = [](const std::vector<uint8_t>& s, uint32_t sw, uint32_t sh,
                  std::vector<uint8_t>& d, uint32_t dw2, uint32_t dh2) {
        d.resize(static_cast<size_t>(dw2) * dh2);
        for (uint32_t y = 0; y < dh2; ++y) {
            const uint32_t sy0 = std::min(y * 2, sh - 1);
            const uint32_t sy1 = std::min(y * 2 + 1, sh - 1);
            const uint8_t* r0 = s.data() + static_cast<size_t>(sy0) * sw;
            const uint8_t* r1 = s.data() + static_cast<size_t>(sy1) * sw;
            uint8_t* out_row = d.data() + static_cast<size_t>(y) * dw2;
            for (uint32_t x = 0; x < dw2; ++x) {
                const uint32_t sx0 = std::min(x * 2, sw - 1);
                const uint32_t sx1 = std::min(x * 2 + 1, sw - 1);
                out_row[x] = static_cast<uint8_t>(
                    (r0[sx0] + r0[sx1] + r1[sx0] + r1[sx1] + 2) / 4);
            }
        }
    };
    box(src.y, src.width, src.height, dst.y, dw, dh);
    box(src.u, scw, sch, dst.u, dcw, dch);
    box(src.v, scw, sch, dst.v, dcw, dch);
}

// Thumbnail strip: fixed-height RGB thumbs appended to a flat
// buffer; written as <stem>.thumbs = 'THM1' u16 w, u16 h, u16 count + RGB.
struct ThumbStrip {
    uint32_t w = 0, h = 36;
    uint32_t count = 0;
    std::vector<uint8_t> rgb;

    void add(const I420Frame& f) {
        if (w == 0)
            w = std::max(8u,
                         (h * f.width / std::max(1u, f.height)) & ~1u);
        const size_t base = rgb.size();
        rgb.resize(base + static_cast<size_t>(w) * h * 3);
        const uint32_t scw = (f.width + 1) / 2;
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t sy = y * f.height / h;
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t sx = x * f.width / w;
                const int Y = f.y[static_cast<size_t>(sy) * f.width + sx];
                const int U =
                    f.u[static_cast<size_t>(sy / 2) * scw + sx / 2] - 128;
                const int V =
                    f.v[static_cast<size_t>(sy / 2) * scw + sx / 2] - 128;
                // BT.709-ish integer conversion — thumbnails, not color
                // science.
                const int r = Y + ((403 * V) >> 8);
                const int g = Y - ((48 * U + 120 * V) >> 8);
                const int b = Y + ((475 * U) >> 8);
                uint8_t* px = rgb.data() + base +
                              (static_cast<size_t>(y) * w + x) * 3;
                px[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
                px[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
                px[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            }
        }
        ++count;
    }

    bool write(const std::filesystem::path& path) const {
        if (count == 0) return false;
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        uint8_t header[10];
        std::memcpy(header, "THM1", 4);
        header[4] = static_cast<uint8_t>(w);
        header[5] = static_cast<uint8_t>(w >> 8);
        header[6] = static_cast<uint8_t>(h);
        header[7] = static_cast<uint8_t>(h >> 8);
        header[8] = static_cast<uint8_t>(count);
        header[9] = static_cast<uint8_t>(count >> 8);
        bool ok = std::fwrite(header, 1, 10, f) == 10 &&
                  std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
        std::fclose(f);
        return ok;
    }
};

// Wall-clock stage accumulator for the import-speed log line.
struct StageClock {
    double seconds = 0.0;
    std::chrono::steady_clock::time_point mark;
    void begin() { mark = std::chrono::steady_clock::now(); }
    void end() {
        seconds += std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - mark)
                       .count();
    }
};

// The import pixel pipeline: double-buffered batches. The decode thread
// pushes raw NV12 frames (ownership moves) and never touches pixels;
// when a batch fills, a background thread fans it out over the worker
// count (convert, proxy downsample, both encodes per frame - the
// atomic-cursor loop) and then appends IN ORDER (main mez, proxy mez,
// thumbs) while the decode thread fills the next batch. The batch is
// sized so cooking a full batch is FASTER than decoding the next one -
// the submit join then never waits and the pixel work hides entirely
// behind the decode. Deliberately batch-synchronous: a persistent
// work-queue variant convoyed on its queue lock and deadlocked; this
// shape is dumb, measured, and correct. Peak transient = two batches
// of raw NV12 (~600 MB at 4K).
class ImportPipeline {
public:
    ImportPipeline(codec::MezWriter& writer, codec::MezWriter* proxy,
                   ThumbStrip* strip, size_t thumb_every, int quality,
                   int threads)
        : writer_(writer), proxy_(proxy), strip_(strip),
          thumb_every_(thumb_every), quality_(quality) {
        // Leave the MF decoder's internal threads a few cores of
        // headroom - starving the pump just moves the wall.
        const unsigned hw =
            std::max(1u, std::thread::hardware_concurrency());
        threads_ = threads > 0
                       ? threads
                       : static_cast<int>(
                             std::max(2u, hw > 4 ? hw - 4 : hw));
    }

    ~ImportPipeline() { join(); }

    void push(platform::VideoFrameNV12 frame) {
        batch_.push_back({std::move(frame), frame_index_++});
        const size_t cap =
            std::min<size_t>(static_cast<size_t>(threads_), 24);
        if (batch_.size() >= cap) submit();
    }

    // Flushes everything and joins; the writers are fully caught up on
    // return. False when any main-mez append failed.
    bool finish() {
        submit();
        join();
        return ok_;
    }

    bool ok() const { return ok_; }
    bool proxy_ok() const { return proxy_ok_; }
    int worker_count() const { return threads_; }
    double pixels_seconds() const { return t_pixels_.seconds; }
    double append_seconds() const { return t_append_.seconds; }

private:
    struct Raw {
        platform::VideoFrameNV12 nv12;
        uint64_t index = 0;
    };
    struct Cooked {
        std::vector<uint8_t> main_payload;
        std::vector<uint8_t> proxy_payload;
        I420Frame thumb_frame;
        bool thumb = false;
    };

    void submit() {
        if (batch_.empty()) return;
        join();   // instant when the batch outpaced the decode fill
        bg_batch_ = std::move(batch_);
        batch_.clear();
        bg_ = std::thread([this] { run_batch(); });
    }

    void join() {
        if (bg_.joinable()) bg_.join();
    }

    void run_batch() {
        std::vector<Cooked> cooked(bg_batch_.size());
        t_pixels_.begin();
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= bg_batch_.size()) break;
                Cooked& c = cooked[i];
                I420Frame f;
                nv12_to_i420(bg_batch_[i].nv12, f);
                bg_batch_[i].nv12 = {};   // 12 MB freed early
                codec::encode_frame(f.view(), quality_, c.main_payload);
                if (proxy_) {
                    I420Frame half;
                    downsample_half(f, half);
                    codec::encode_frame(half.view(), quality_,
                                        c.proxy_payload);
                }
                if (thumb_every_ &&
                    bg_batch_[i].index % thumb_every_ == 0) {
                    c.thumb = true;
                    c.thumb_frame = std::move(f);
                }
            }
        };
        std::vector<std::thread> pool;
        const int n =
            std::min<int>(threads_, static_cast<int>(bg_batch_.size()));
        pool.reserve(static_cast<size_t>(n) - 1);
        for (int i = 1; i < n; ++i) pool.emplace_back(worker);
        worker();
        for (std::thread& t : pool) t.join();
        t_pixels_.end();

        t_append_.begin();
        for (Cooked& c : cooked) {
            ok_ = ok_ && writer_.add_encoded_frame(c.main_payload);
            if (proxy_ && proxy_ok_)
                proxy_ok_ = proxy_->add_encoded_frame(c.proxy_payload);
            if (c.thumb && strip_) strip_->add(c.thumb_frame);
        }
        t_append_.end();
        bg_batch_.clear();
    }

    codec::MezWriter& writer_;
    codec::MezWriter* proxy_;
    ThumbStrip* strip_;
    size_t thumb_every_;
    uint64_t frame_index_ = 0;
    int quality_;
    int threads_ = 1;
    std::vector<Raw> batch_;
    std::vector<Raw> bg_batch_;   // owned by bg_ while it runs
    std::thread bg_;
    bool ok_ = true;         // bg-thread written, read after join only
    bool proxy_ok_ = true;
    StageClock t_pixels_;    // parallel convert+downsample+encode wall
    StageClock t_append_;    // ordered writes + thumb adds
};

bool import_video(BmffFile& file, const TrackInfo& track,
                  const std::filesystem::path& mez_path,
                  const ImportOptions& options, ImportProgress* progress,
                  ImportResult* result, mod::VideoAnalyzer* analyzer) {
    platform::H264Decoder decoder;
    std::string error;
    // Software decode for import: every frame is consumed on the CPU, and
    // the DXVA path's per-frame sync readback stall (~8 ms flat) costs more
    // than the multithreaded software decoder at any resolution.
    if (!decoder.create(track.avcc, track.width, track.height, &error,
                        /*allow_d3d=*/false)) {
        result->error = "video decoder: " + error;
        return false;
    }

    // Frame duration: assume CFR from the first sample (VFR sources get
    // resampled to this grid implicitly — acceptable for v1).
    const uint32_t frame_duration =
        track.samples.empty() ? 1 : std::max(1u, track.samples[0].duration);

    codec::MezWriter writer;
    if (!writer.open(mez_path, track.width, track.height, track.timescale,
                     frame_duration, options.quality)) {
        result->error = "cannot create " + mez_path.string();
        return false;
    }

    if (progress)
        progress->frames_total.store(static_cast<uint32_t>(track.samples.size()));

    // Half-res proxy: second mezzanine, same timeline.
    codec::MezWriter proxy_writer;
    const std::filesystem::path proxy_path = [&] {
        std::filesystem::path p = mez_path;
        p.replace_extension(".proxy.mez");
        return p;
    }();
    const uint32_t proxy_w = std::max(2u, (track.width / 2) & ~1u);
    const uint32_t proxy_h = std::max(2u, (track.height / 2) & ~1u);
    const bool proxy_on =
        options.proxy && proxy_writer.open(proxy_path, proxy_w, proxy_h,
                                           track.timescale, frame_duration,
                                           options.quality);

    // Thumbnail strip: every Nth frame, budgeted count.
    ThumbStrip strip;
    const size_t thumb_every =
        options.thumb_count > 0
            ? std::max<size_t>(1, track.samples.size() /
                                      static_cast<size_t>(options.thumb_count))
            : 0;

    ImportPipeline pipeline(writer, proxy_on ? &proxy_writer : nullptr,
                            thumb_every ? &strip : nullptr, thumb_every,
                            options.quality, options.encode_threads);

    std::vector<uint8_t> sample_bytes;
    platform::VideoFrameNV12 nv12;
    uint32_t decoded = 0;
    StageClock t_decode, t_analyze;
    const auto t_start = std::chrono::steady_clock::now();

    auto pump_decoder = [&]() {
        for (;;) {
            t_decode.begin();
            const bool got = decoder.receive(nv12);
            t_decode.end();
            if (!got) break;
            // The analyzer reads the luma plane straight off the NV12
            // (identical bytes to the converted Y, so the curves stay
            // bit-identical) - the decode thread hands the frame to the
            // pipeline untouched and pixels never run serially here.
            if (analyzer) {
                t_analyze.begin();
                analyzer->push_frame(nv12.data.data(), nv12.width,
                                     nv12.width, nv12.height);
                t_analyze.end();
            }
            pipeline.push(std::move(nv12));
            ++decoded;
            if (progress) progress->frames_done.store(decoded);
        }
    };

    const double to_100ns = 1.0e7 / track.timescale;
    for (const SampleInfo& sample : track.samples) {
        if (progress && progress->cancel.load()) {
            result->error = "cancelled";
            return false;
        }
        if (!file.read_sample(sample, sample_bytes)) {
            result->error = "sample read failed";
            return false;
        }
        const int64_t pts = static_cast<int64_t>(
            static_cast<double>(static_cast<int64_t>(sample.dts) +
                                sample.cts_offset) * to_100ns);
        const int64_t duration =
            static_cast<int64_t>(sample.duration * to_100ns);
        t_decode.begin();
        const bool fed = decoder.feed(sample_bytes.data(), sample_bytes.size(),
                                      pts, duration, sample.keyframe);
        t_decode.end();
        if (!fed) {
            result->error = "video decode failed";
            return false;
        }
        pump_decoder();
    }
    decoder.drain();
    pump_decoder();
    const bool pipeline_ok = pipeline.finish();

    // Stage split for the speed target (import ≥ 2× realtime). Decode
    // and pixels overlap by design, so the stages sum past the total
    // once the overlap is winning.
    const double t_total =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      t_start)
            .count();
    log_info("import: video stages %.2fs total — decode %.2fs, analyze "
             "%.2fs, pixels %.2fs / %d workers, append %.2fs "
             "(%u frames, %.1f fps)",
             t_total, t_decode.seconds, t_analyze.seconds,
             pipeline.pixels_seconds(), pipeline.worker_count(),
             pipeline.append_seconds(), decoded,
             t_total > 0.0 ? decoded / t_total : 0.0);

    if (!pipeline_ok) {
        result->error = "mezzanine encode/write failed";
        return false;
    }
    if (!writer.finish()) {
        result->error = "mez finalize failed";
        return false;
    }
    if (proxy_on) {
        if (pipeline.proxy_ok() && proxy_writer.finish())
            result->proxy_path = proxy_path;
        else
            log_warn("import: proxy encode failed — full-res only");
    }
    if (thumb_every) {
        std::filesystem::path tpath = mez_path;
        tpath.replace_extension(".thumbs");
        if (strip.write(tpath)) result->thumbs_path = tpath;
    }
    if (decoded == 0) {
        result->error = "no frames decoded";
        return false;
    }

    result->mez_path = mez_path;
    result->width = track.width;
    result->height = track.height;
    result->frame_count = writer.frame_count();
    result->fps = frame_duration
        ? static_cast<double>(track.timescale) / frame_duration : 0.0;
    return true;
}

bool import_audio(BmffFile& file, const TrackInfo& track,
                  const std::filesystem::path& pcm_path,
                  ImportResult* result) {
    platform::AacDecoder decoder;
    std::string error;
    if (!decoder.create(track.audio_specific_config, track.channels,
                        track.sample_rate, &error)) {
        result->error = "audio decoder: " + error;
        return false;
    }

    PcmWriter writer;
    bool writer_open = false;
    std::vector<uint8_t> sample_bytes;
    platform::AudioChunk chunk;

    auto pump = [&]() -> bool {
        while (decoder.receive(chunk)) {
            if (!writer_open) {
                if (!writer.open(pcm_path, chunk.channels, chunk.sample_rate)) {
                    result->error = "cannot create " + pcm_path.string();
                    return false;
                }
                writer_open = true;
                result->audio_channels = chunk.channels;
                result->audio_sample_rate = chunk.sample_rate;
            }
            if (!writer.append(chunk.samples.data(), chunk.samples.size())) {
                result->error = "pcm write failed";
                return false;
            }
        }
        return true;
    };

    const double to_100ns = 1.0e7 / track.timescale;
    for (const SampleInfo& sample : track.samples) {
        if (!file.read_sample(sample, sample_bytes)) {
            result->error = "audio sample read failed";
            return false;
        }
        const int64_t pts =
            static_cast<int64_t>(static_cast<double>(sample.dts) * to_100ns);
        if (!decoder.feed(sample_bytes.data(), sample_bytes.size(), pts)) {
            result->error = "audio decode failed";
            return false;
        }
        if (!pump()) return false;
    }
    decoder.drain();
    if (!pump()) return false;

    if (writer_open) {
        writer.finish();
        result->pcm_path = pcm_path;
        result->audio_frames = writer.frames_written();
    }
    return true;
}

// Still-image import (PNG/TGA through the in-repo decoders, ):
// the image is encoded ONCE and every timeline frame's index entry points
// at that payload — a 10-second stream for one frame of storage. Downstream
// the bundle is indistinguishable from footage, so the whole rack (mattes,
// modulation, trim, export) works on stills untouched.
constexpr uint32_t kStillFps = 30;
constexpr uint32_t kStillFrames = 300;   // 10 s at 30 fps

// Writes a one-image bundle: the frame encoded once, every index entry
// pointing at that payload, plus proxy/thumb/analysis sidecars. Shared
// by still imports and audio cover art (held for the audio's length).
bool write_still_bundle(const ImageRgba& img,
                        const std::filesystem::path& mez_path,
                        const ImportOptions& options, uint32_t hold_frames,
                        ImportResult* result) {
    // sRGB RGB -> BT.709 limited-range I420: the exact inverse of the
    // engine's frame-fetch matrix (ycbcr_to_rgb), so a still round-trips
    // through preview/export with no color shift. Even dims for the codec
    // and NV12 paths.
    I420Frame frame;
    frame.width = std::max(2u, img.width & ~1u);
    frame.height = std::max(2u, img.height & ~1u);
    const uint32_t cw = (frame.width + 1) / 2;
    const uint32_t ch = (frame.height + 1) / 2;
    frame.y.resize(static_cast<size_t>(frame.width) * frame.height);
    frame.u.resize(static_cast<size_t>(cw) * ch);
    frame.v.resize(static_cast<size_t>(cw) * ch);
    auto to_u8 = [](float v) {
        return static_cast<uint8_t>(
            std::clamp(static_cast<int>(v + 0.5f), 0, 255));
    };
    double luma_sum = 0.0;
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint8_t* px =
                img.pixels.data() +
                (static_cast<size_t>(y) * img.width + x) * 4;
            const float yf = (0.2126f * px[0] + 0.7152f * px[1] +
                              0.0722f * px[2]) /
                             255.0f;
            frame.y[static_cast<size_t>(y) * frame.width + x] =
                to_u8(16.0f + 219.0f * yf);
            luma_sum += yf;
        }
    }
    for (uint32_t cy = 0; cy < ch; ++cy) {
        for (uint32_t cx = 0; cx < cw; ++cx) {
            // Box-average the 2x2 RGB block, then convert.
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (uint32_t sy = 0; sy < 2; ++sy)
                for (uint32_t sx = 0; sx < 2; ++sx) {
                    const uint32_t ix =
                        std::min(cx * 2 + sx, frame.width - 1);
                    const uint32_t iy =
                        std::min(cy * 2 + sy, frame.height - 1);
                    const uint8_t* px =
                        img.pixels.data() +
                        (static_cast<size_t>(iy) * img.width + ix) * 4;
                    r += px[0];
                    g += px[1];
                    b += px[2];
                }
            r *= 0.25f / 255.0f;
            g *= 0.25f / 255.0f;
            b *= 0.25f / 255.0f;
            const float yf = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            frame.u[static_cast<size_t>(cy) * cw + cx] =
                to_u8(128.0f + 224.0f * (b - yf) / 1.8556f);
            frame.v[static_cast<size_t>(cy) * cw + cx] =
                to_u8(128.0f + 224.0f * (r - yf) / 1.5748f);
        }
    }

    codec::MezWriter writer;
    if (!writer.open(mez_path, frame.width, frame.height, kStillFps * 1000,
                     1000, options.quality)) {
        result->error = "cannot create " + mez_path.string();
        return false;
    }
    if (!writer.add_frame(frame.view()) ||
        !writer.add_hold_frames(hold_frames - 1) || !writer.finish()) {
        result->error = "mezzanine write failed";
        return false;
    }

    // Half-res proxy: same one-payload trick.
    if (options.proxy) {
        I420Frame half;
        downsample_half(frame, half);
        std::filesystem::path proxy_path = mez_path;
        proxy_path.replace_extension(".proxy.mez");
        codec::MezWriter proxy;
        if (proxy.open(proxy_path, half.width, half.height, kStillFps * 1000,
                       1000, options.quality) &&
            proxy.add_frame(half.view()) &&
            proxy.add_hold_frames(hold_frames - 1) && proxy.finish())
            result->proxy_path = proxy_path;
    }

    // One-thumb filmstrip; the ruler stretches it across the timeline.
    if (options.thumb_count > 0) {
        ThumbStrip strip;
        strip.add(frame);
        std::filesystem::path tpath = mez_path;
        tpath.replace_extension(".thumbs");
        if (strip.write(tpath)) result->thumbs_path = tpath;
    }

    // Analysis: constant brightness, zero motion/cuts — deterministic
    // and honest for a static image.
    {
        mod::AnalysisData data;
        data.fps = kStillFps;
        data.frame_count = hold_frames;
        const float mean = static_cast<float>(
            luma_sum / (static_cast<double>(frame.width) * frame.height));
        data.brightness.assign(hold_frames, mean);
        data.motion.assign(hold_frames, 0.0f);
        data.cut.assign(hold_frames, 0.0f);
        std::filesystem::path apath = mez_path;
        apath.replace_extension(".analysis");
        if (mod::write_analysis(apath, data))
            result->analysis_path = apath;
    }

    result->ok = true;
    result->mez_path = mez_path;
    result->width = frame.width;
    result->height = frame.height;
    result->frame_count = hold_frames;
    result->fps = kStillFps;
    log_info("import: still %ux%u -> %u held frames", frame.width,
             frame.height, hold_frames);
    return true;
}

bool import_still(const std::filesystem::path& source,
                  const std::filesystem::path& dest_dir,
                  const ImportOptions& options, ImportProgress* progress,
                  ImportResult* result) {
    ImageRgba img;
    std::string error;
    if (!load_image(source, &img, &error)) {
        result->error = "image: " + error;
        return false;
    }
    if (progress) {
        progress->frames_total.store(1);
        progress->frames_done.store(0);
    }
    const bool ok = write_still_bundle(
        img, dest_dir / (source.stem().wstring() + L".mez"), options,
        kStillFrames, result);
    if (ok && progress) progress->frames_done.store(1);
    return ok;
}

// Audio import (WAV via the in-repo reader, MP3 via the in-repo frame
// walker over the inbox MFT): PCM sidecar always; embedded cover art
// additionally becomes the VIDEO side - a one-image mezzanine held for
// the audio's length, so the media node shows the art and plays the
// sound. Without art there is no mez: the node is image-dormant and
// asset frame_count stays 0 = unbounded, like a generator.
bool import_audio_file(const std::filesystem::path& source,
                       const std::filesystem::path& dest_dir,
                       const ImportOptions& options,
                       ImportProgress* progress, ImportResult* result) {
    std::wstring ext = source.extension().native();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));

    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> samples;
    std::vector<uint8_t> art;
    std::string error;
    if (ext == L".wav") {
        WavData wav;
        if (!read_wav(source, &wav, &error)) {
            result->error = "wav: " + error;
            return false;
        }
        channels = wav.channels;
        rate = wav.sample_rate;
        samples = std::move(wav.samples);
    } else {
        FILE* f = _wfopen(source.c_str(), L"rb");
        if (!f) {
            result->error = "cannot open " + source.string();
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> bytes(len > 0 ? static_cast<size_t>(len) : 0);
        const size_t got =
            bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (got != bytes.size() || bytes.empty()) {
            result->error = "short read";
            return false;
        }
        Mp3Data mp3;
        if (!decode_mp3(bytes.data(), bytes.size(), &mp3, &error)) {
            result->error = "mp3: " + error;
            return false;
        }
        channels = mp3.channels;
        rate = mp3.sample_rate;
        samples = std::move(mp3.samples);
        mp3_cover_art(bytes.data(), bytes.size(), &art);
    }
    if (progress) {
        progress->frames_total.store(1);
        progress->frames_done.store(0);
    }

    const std::filesystem::path pcm_path =
        dest_dir / (source.stem().wstring() + L".pcm");
    PcmWriter writer;
    if (!writer.open(pcm_path, channels, rate)) {
        result->error = "cannot create " + pcm_path.string();
        return false;
    }
    if (!writer.append(samples.data(), samples.size())) {
        result->error = "pcm write failed";
        return false;
    }
    writer.finish();
    result->pcm_path = pcm_path;
    result->audio_channels = channels;
    result->audio_sample_rate = rate;
    result->audio_frames =
        channels ? samples.size() / channels : 0;

    if (!art.empty()) {
        ImageRgba img;
        std::string wic_error;
        if (platform::decode_image_rgba(art.data(), art.size(), &img.width,
                                        &img.height, &img.pixels,
                                        &wic_error)) {
            // Hold the art for the audio's length on the still grid.
            const double secs =
                rate ? static_cast<double>(result->audio_frames) / rate : 0.0;
            const uint32_t hold = std::max<uint32_t>(
                1, static_cast<uint32_t>(secs * kStillFps) + 1);
            ImportResult art_result;
            if (write_still_bundle(
                    img, dest_dir / (source.stem().wstring() + L".mez"),
                    options, hold, &art_result)) {
                result->mez_path = art_result.mez_path;
                result->proxy_path = art_result.proxy_path;
                result->thumbs_path = art_result.thumbs_path;
                result->analysis_path = art_result.analysis_path;
                result->width = art_result.width;
                result->height = art_result.height;
                result->frame_count = art_result.frame_count;
                result->fps = art_result.fps;
            }
        } else {
            log_warn("import: cover art undecodable (%s) — audio only",
                     wic_error.c_str());
        }
    }

    if (progress) progress->frames_done.store(1);
    result->ok = true;
    log_info("import: audio %u ch @%u Hz, %llu frames%s", channels, rate,
             static_cast<unsigned long long>(result->audio_frames),
             result->mez_path.empty() ? "" : " + cover art");
    return true;
}

}  // namespace

double probe_video_duration_seconds(const std::filesystem::path& source) {
    FILE* f = _wfopen(source.c_str(), L"rb");
    if (!f) return 0.0;
    _fseeki64(f, 0, SEEK_END);
    const int64_t file_end = _ftelli64(f);
    auto be32 = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | p[3];
    };
    auto be64 = [&](const uint8_t* p) {
        return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
    };
    // Finds a child box inside [begin, end); returns payload begin/end.
    auto find_box = [&](int64_t begin, int64_t end, const char* fourcc,
                        int64_t* pb, int64_t* pe) {
        int64_t at = begin;
        uint8_t hdr[16];
        for (int guard = 0; guard < 4096 && at + 8 <= end; ++guard) {
            _fseeki64(f, at, SEEK_SET);
            if (std::fread(hdr, 1, 8, f) != 8) return false;
            uint64_t size = be32(hdr);
            int64_t payload = at + 8;
            if (size == 1) {
                if (std::fread(hdr + 8, 1, 8, f) != 8) return false;
                size = be64(hdr + 8);
                payload = at + 16;
            } else if (size == 0) {
                size = static_cast<uint64_t>(end - at);
            }
            if (size < 8) return false;
            if (!std::memcmp(hdr + 4, fourcc, 4)) {
                *pb = payload;
                *pe = at + static_cast<int64_t>(size);
                return true;
            }
            at += static_cast<int64_t>(size);
        }
        return false;
    };
    double secs = 0.0;
    int64_t moov_b = 0, moov_e = 0;
    if (find_box(0, file_end, "moov", &moov_b, &moov_e)) {
        // Walk every trak; the VIDEO handler's mdhd is the duration the
        // mezzanine must cover (mvhd would count a longer audio tail
        // and force a re-import loop).
        int64_t at = moov_b;
        int64_t trak_b = 0, trak_e = 0;
        while (find_box(at, moov_e, "trak", &trak_b, &trak_e)) {
            int64_t mdia_b = 0, mdia_e = 0;
            if (find_box(trak_b, trak_e, "mdia", &mdia_b, &mdia_e)) {
                int64_t hb = 0, he = 0;
                uint8_t buf[36];
                bool is_video = false;
                if (find_box(mdia_b, mdia_e, "hdlr", &hb, &he)) {
                    _fseeki64(f, hb, SEEK_SET);
                    if (std::fread(buf, 1, 12, f) == 12)
                        is_video = !std::memcmp(buf + 8, "vide", 4);
                }
                if (is_video &&
                    find_box(mdia_b, mdia_e, "mdhd", &hb, &he)) {
                    _fseeki64(f, hb, SEEK_SET);
                    if (std::fread(buf, 1, 32, f) == 32) {
                        uint32_t ts = 0;
                        uint64_t dur = 0;
                        if (buf[0] == 1) {
                            ts = be32(buf + 20);
                            dur = be64(buf + 24);
                        } else {
                            ts = be32(buf + 12);
                            dur = be32(buf + 16);
                        }
                        if (ts) {
                            secs = static_cast<double>(dur) / ts;
                            break;
                        }
                    }
                }
            }
            at = trak_e;
        }
    }
    std::fclose(f);
    return secs;
}

bool extract_audio_pcm(const std::filesystem::path& source,
                       const std::filesystem::path& dest_pcm,
                       std::string* error) {
    auto fail = [&](std::string what) {
        if (error) *error = std::move(what);
        return false;
    };
    std::wstring ext = source.extension().native();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));

    if (ext == L".wav") {
        WavData wav;
        std::string wav_error;
        if (!read_wav(source, &wav, &wav_error))
            return fail("wav: " + wav_error);
        PcmWriter writer;
        if (!writer.open(dest_pcm, wav.channels, wav.sample_rate))
            return fail("cannot create " + dest_pcm.string());
        if (!writer.append(wav.samples.data(), wav.samples.size()))
            return fail("pcm write failed");
        writer.finish();
        return true;
    }
    if (ext == L".mp3") {
        Mp3Data mp3;
        std::string mp3_error;
        if (!read_mp3(source, &mp3, &mp3_error))
            return fail("mp3: " + mp3_error);
        PcmWriter writer;
        if (!writer.open(dest_pcm, mp3.channels, mp3.sample_rate))
            return fail("cannot create " + dest_pcm.string());
        if (!writer.append(mp3.samples.data(), mp3.samples.size()))
            return fail("pcm write failed");
        writer.finish();
        return true;
    }

    platform::MfSession session;
    if (!session.ok()) return fail("Media Foundation unavailable");
    BmffFile file;
    std::string demux_error;
    if (!file.open(source, &demux_error))
        return fail("demux: " + demux_error);
    const TrackInfo* audio = file.movie().first_audio();
    if (!audio || std::string(audio->fourcc) != "mp4a" ||
        audio->samples.empty() || audio->audio_specific_config.empty())
        return fail("no AAC audio track");
    ImportResult scratch;
    if (!import_audio(file, *audio, dest_pcm, &scratch))
        return fail(scratch.error);
    if (scratch.pcm_path.empty()) return fail("audio track decoded empty");
    return true;
}

ImportResult import_media(const std::filesystem::path& source,
                          const std::filesystem::path& dest_dir,
                          const ImportOptions& options,
                          ImportProgress* progress) {
    ImportResult result;

    // Still images (PNG/TGA) skip Media Foundation entirely — the in-repo
    // decoders and the mezzanine writer are all it takes.
    std::wstring ext = source.extension().native();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext == L".png" || ext == L".tga") {
        import_still(source, dest_dir, options, progress, &result);
        return result;
    }
    if (ext == L".wav" || ext == L".mp3") {
        std::error_code wec;
        std::filesystem::create_directories(dest_dir, wec);
        import_audio_file(source, dest_dir, options, progress, &result);
        return result;
    }

    platform::MfSession session;
    if (!session.ok()) {
        result.error = "Media Foundation unavailable";
        return result;
    }

    BmffFile file;
    std::string error;
    if (!file.open(source, &error)) {
        result.error = "demux: " + error;
        return result;
    }

    const TrackInfo* video = file.movie().first_video();
    if (!video || std::string(video->fourcc) != "avc1") {
        result.error = "no H.264 video track";
        return result;
    }
    if (video->samples.empty()) {
        result.error = "empty video track";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    const std::wstring stem = source.stem().wstring();

    mod::VideoAnalyzer video_analyzer;
    if (!import_video(file, *video, dest_dir / (stem + L".mez"), options,
                      progress, &result, &video_analyzer))
        return result;

    const TrackInfo* audio = file.movie().first_audio();
    if (audio && std::string(audio->fourcc) == "mp4a" &&
        !audio->samples.empty() && !audio->audio_specific_config.empty()) {
        if (!import_audio(file, *audio, dest_dir / (stem + L".pcm"), &result))
            return result;
    }

    // ---- analysis sidecar: video curves from the decode pass,
    // audio curves from the PCM sidecar we just wrote.
    {
        mod::AnalysisData analysis;
        analysis.fps = result.fps;
        analysis.frame_count = result.frame_count;
        video_analyzer.finish(&analysis);
        if (!result.pcm_path.empty()) {
            PcmReader pcm;
            std::string pcm_error;
            if (pcm.open(result.pcm_path, &pcm_error)) {
                std::vector<int16_t> samples(
                    static_cast<size_t>(pcm.frame_count()) * pcm.channels());
                pcm.read(0, samples.data(),
                         static_cast<size_t>(pcm.frame_count()));
                mod::analyze_audio(samples.data(), pcm.frame_count(),
                                   pcm.channels(), pcm.sample_rate(),
                                   result.fps, result.frame_count, &analysis);
            }
        }
        const std::filesystem::path analysis_path =
            dest_dir / (stem + L".analysis");
        if (mod::write_analysis(analysis_path, analysis))
            result.analysis_path = analysis_path;
        else
            log_warn("import: analysis write failed (non-fatal)");
    }

    result.ok = true;
    log_info("import: %s -> %u frames @%0.3f fps%s", source.string().c_str(),
             result.frame_count, result.fps,
             result.pcm_path.empty() ? "" : " + audio");
    return result;
}

}  // namespace looks::media
