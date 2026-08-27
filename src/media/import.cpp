#include "media/import.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "codec/mez.h"
#include "media/audio_mix.h"
#include "media/bmff.h"
#include "media/bundle.h"
#include "media/export.h"
#include "media/mp3.h"
#include "media/pcm.h"
#include "media/thumbs.h"
#include "media/wav.h"
#include "mod/analysis.h"
#include "platform/win/mf_codec.h"
#include "platform/win/wic_image.h"
#include "util/color.h"
#include "util/file.h"
#include "util/image.h"
#include "util/log.h"

namespace looks::media {

namespace {

std::wstring lower_ext(const std::filesystem::path& p) {
    std::wstring ext = p.extension().native();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

// An AAC audio track the importer can decode.
bool aac_track(const TrackInfo* t) {
    return t && std::string(t->fourcc) == "mp4a" && !t->samples.empty() &&
           !t->audio_specific_config.empty();
}

// Container-level video facts every native import path stamps.
void stamp_video_facts(const TrackInfo& video, ImportResult* result) {
    result->width = video.width;
    result->height = video.height;
    result->frame_count = static_cast<uint32_t>(video.samples.size());
    const uint32_t frame_duration = std::max(1u, video.samples[0].duration);
    result->fps = static_cast<double>(video.timescale) / frame_duration;
}

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

// Thumbnail strip builder over the shared .thumbs sidecar
// (media/thumbs.h). Each destination pixel box-averages its source
// region - a 1920 -> 160 px point sample would alias detail into noise.
struct ThumbStrip {
    ThumbStripData data{0, kThumbStripH, 0, {}};

    void add(const I420Frame& f) {
        uint32_t& w = data.w;
        const uint32_t h = data.h;
        if (w == 0)
            w = std::max(8u,
                         (h * f.width / std::max(1u, f.height)) & ~1u);
        const size_t base = data.rgb.size();
        data.rgb.resize(base + static_cast<size_t>(w) * h * 3);
        const uint32_t scw = (f.width + 1) / 2;
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t sy0 = y * f.height / h;
            const uint32_t sy1 =
                std::max(sy0 + 1, (y + 1) * f.height / h);
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t sx0 = x * f.width / w;
                const uint32_t sx1 =
                    std::max(sx0 + 1, (x + 1) * f.width / w);
                uint32_t sum_y = 0, sum_u = 0, sum_v = 0, n = 0;
                for (uint32_t sy = sy0; sy < sy1; ++sy)
                    for (uint32_t sx = sx0; sx < sx1; ++sx) {
                        sum_y +=
                            f.y[static_cast<size_t>(sy) * f.width + sx];
                        sum_u += f.u[static_cast<size_t>(sy / 2) * scw +
                                     sx / 2];
                        sum_v += f.v[static_cast<size_t>(sy / 2) * scw +
                                     sx / 2];
                        ++n;
                    }
                int r, g, b;
                color::ycbcr709_to_rgb8(
                    static_cast<int>((sum_y + n / 2) / n),
                    static_cast<int>((sum_u + n / 2) / n),
                    static_cast<int>((sum_v + n / 2) / n), &r, &g, &b);
                uint8_t* px = data.rgb.data() + base +
                              (static_cast<size_t>(y) * w + x) * 3;
                px[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
                px[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
                px[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            }
        }
        ++data.count;
    }

    bool write(const std::filesystem::path& path) const {
        return write_thumbs(path, data);
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

// The background half of video ingest: ONE software decode over the
// whole track, feeding the motion/brightness/cut curves and the
// thumbnail strip - the only stages that still need every pixel now that
// playback decodes the source natively. Nothing is written until the
// pass completes, so a cancel leaves the fast-stage sidecars intact and
// the asset stays usable (curves and thumbs just never land).
bool ingest_video_pass(BmffFile& file, const TrackInfo& track,
                       const ImportOptions& options,
                       ImportProgress* progress, mod::AnalysisData* analysis,
                       const std::filesystem::path& dest_dir,
                       const std::wstring& stem, ImportResult* result) {
    platform::H264Decoder decoder;
    std::string error;
    // Software decode: every frame is consumed on the CPU, and the DXVA
    // path's per-frame sync readback stall (~8 ms flat) costs more than
    // the multithreaded software decoder at any resolution. LOW LATENCY
    // is load-bearing, not a tuning: without it the software MFT
    // schedules decode through the process-shared MF work queues, and
    // with a preview pool's worth of hardware sessions live those can
    // starve it PERMANENTLY - ProcessOutput then parks forever with
    // zero CPU (ignoring the cancel poll around it) and the app hangs
    // at close joining this thread. Synchronous per-frame completion
    // never touches the shared queues, and a sequential pass loses
    // nothing to it.
    if (!decoder.create(track.avcc, track.width, track.height, &error,
                        /*allow_d3d=*/false, /*low_latency=*/true)) {
        log_warn("ingest: video pass decoder failed (%s)", error.c_str());
        return false;
    }
    // Stage breadcrumb: this pass runs on a background thread that the
    // app JOINS at close - a wedge before the frame loop otherwise
    // reads as a silent closing hang with no stage to blame.
    log_info("ingest: pass decoder up (%zu samples)", track.samples.size());
    if (progress)
        progress->frames_total.store(
            static_cast<uint32_t>(track.samples.size()));

    mod::VideoAnalyzer analyzer;
    ThumbStrip strip;
    const size_t thumb_every =
        options.thumb_count > 0
            ? std::max<size_t>(1, track.samples.size() /
                                      static_cast<size_t>(options.thumb_count))
            : 0;

    std::vector<uint8_t> sample_bytes;
    platform::VideoFrameNV12 nv12;
    I420Frame thumb_frame;
    uint32_t decoded = 0;
    StageClock t_pass;
    t_pass.begin();

    auto pump_decoder = [&]() {
        while (decoder.receive(nv12)) {
            if (decoded == 0) log_info("ingest: pass first frame");
            // The analyzer reads the luma plane straight off the NV12
            // (identical bytes to a converted Y plane, so the curves
            // match what the old transcode-time analysis produced).
            analyzer.push_frame(nv12.data.data(), nv12.width, nv12.width,
                                nv12.height);
            if (thumb_every && decoded % thumb_every == 0) {
                nv12_to_i420(nv12, thumb_frame);
                strip.add(thumb_frame);
            }
            ++decoded;
            if (progress) progress->frames_done.store(decoded);
        }
    };

    const double to_100ns = 1.0e7 / track.timescale;
    size_t fed = 0;
    for (const SampleInfo& sample : track.samples) {
        if (progress && progress->cancel.load()) {
            log_info("ingest: video pass cancelled — curves/thumbs skipped");
            return false;
        }
        if (++fed % 1000 == 0)
            log_info("ingest: video pass %zu/%zu", fed,
                     track.samples.size());
        if (progress) progress->stage.store(1, std::memory_order_relaxed);
        if (!file.read_sample(sample, sample_bytes)) {
            log_warn("ingest: sample read failed — curves/thumbs skipped");
            return false;
        }
        const int64_t pts = static_cast<int64_t>(
            static_cast<double>(static_cast<int64_t>(sample.dts) +
                                sample.cts_offset) * to_100ns);
        const int64_t duration =
            static_cast<int64_t>(sample.duration * to_100ns);
        if (progress) progress->stage.store(2, std::memory_order_relaxed);
        if (!decoder.feed(sample_bytes.data(), sample_bytes.size(), pts,
                          duration, sample.keyframe)) {
            log_warn("ingest: video decode failed — curves/thumbs skipped");
            return false;
        }
        if (progress) progress->stage.store(3, std::memory_order_relaxed);
        pump_decoder();
        if (progress) progress->stage.store(0, std::memory_order_relaxed);
    }
    decoder.drain();
    pump_decoder();
    t_pass.end();
    if (decoded == 0) return false;

    // Video curves join the audio set already on disk; one writer, whole
    // rewrite, so a reader sees old-complete or new-complete.
    analyzer.finish(analysis);
    const SidecarPaths sc = sidecars_for_stem(dest_dir, stem);
    if (mod::write_analysis(sc.analysis, *analysis))
        result->analysis_path = sc.analysis;
    else
        log_warn("ingest: analysis rewrite failed (non-fatal)");
    if (thumb_every) {
        if (strip.write(sc.thumbs)) result->thumbs_path = sc.thumbs;
    }
    log_info("ingest: video pass %.2fs (%u frames, %.1f fps)",
             t_pass.seconds, decoded,
             t_pass.seconds > 0.0 ? decoded / t_pass.seconds : 0.0);
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
                    result->error = "cannot create " + path_to_u8(pcm_path);
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
bool write_still_bundle(const ImageRgba& img, const SidecarPaths& sc,
                        const ImportOptions& options, uint32_t hold_frames,
                        ImportResult* result) {
    const std::filesystem::path& mez_path = sc.mez;
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
            const float yf =
                color::luma709(px[0], px[1], px[2]) / 255.0f;
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
            const float yf = color::luma709(r, g, b);
            frame.u[static_cast<size_t>(cy) * cw + cx] =
                to_u8(128.0f + 224.0f * (b - yf) / color::kCb709);
            frame.v[static_cast<size_t>(cy) * cw + cx] =
                to_u8(128.0f + 224.0f * (r - yf) / color::kCr709);
        }
    }

    codec::MezWriter writer;
    if (!writer.open(mez_path, frame.width, frame.height, kStillFps * 1000,
                     1000, options.quality)) {
        result->error = "cannot create " + path_to_u8(mez_path);
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
        codec::MezWriter proxy;
        if (proxy.open(sc.proxy, half.width, half.height, kStillFps * 1000,
                       1000, options.quality) &&
            proxy.add_frame(half.view()) &&
            proxy.add_hold_frames(hold_frames - 1) && proxy.finish())
            result->proxy_path = sc.proxy;
    }

    // One-thumb filmstrip; the ruler stretches it across the timeline.
    if (options.thumb_count > 0) {
        ThumbStrip strip;
        strip.add(frame);
        if (strip.write(sc.thumbs)) result->thumbs_path = sc.thumbs;
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
        if (mod::write_analysis(sc.analysis, data))
            result->analysis_path = sc.analysis;
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
        // WIC fallback: variants the in-repo decoders refuse (16-bit,
        // palette, interlaced) import through the same decoder cover
        // art already uses - one acceptance set for every image path.
        const auto bytes = read_file_bytes(source);
        if (!bytes ||
            !platform::decode_image_rgba(bytes->data(), bytes->size(),
                                         &img.width, &img.height,
                                         &img.pixels, &error)) {
            result->error = "image: " + error;
            return false;
        }
    }
    if (progress) {
        progress->frames_total.store(1);
        progress->frames_done.store(0);
    }
    const bool ok = write_still_bundle(img, sidecars_for(dest_dir, source),
                                       options, kStillFrames, result);
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
    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> samples;
    std::vector<uint8_t> art;
    std::string error;
    if (lower_ext(source) == L".wav") {
        WavData wav;
        if (!read_wav(source, &wav, &error)) {
            result->error = "wav: " + error;
            return false;
        }
        channels = wav.channels;
        rate = wav.sample_rate;
        samples = std::move(wav.samples);
    } else {
        const auto bytes = read_file_bytes(source);
        if (!bytes) {
            result->error = "cannot open " + path_to_u8(source);
            return false;
        }
        if (bytes->empty()) {
            result->error = "empty file";
            return false;
        }
        Mp3Data mp3;
        if (!decode_mp3(bytes->data(), bytes->size(), &mp3, &error)) {
            result->error = "mp3: " + error;
            return false;
        }
        channels = mp3.channels;
        rate = mp3.sample_rate;
        samples = std::move(mp3.samples);
        mp3_cover_art(bytes->data(), bytes->size(), &art);
    }
    if (progress) {
        progress->frames_total.store(1);
        progress->frames_done.store(0);
    }

    const SidecarPaths sc = sidecars_for(dest_dir, source);
    PcmWriter writer;
    if (!writer.open(sc.pcm, channels, rate)) {
        result->error = "cannot create " + path_to_u8(sc.pcm);
        return false;
    }
    if (!writer.append(samples.data(), samples.size())) {
        result->error = "pcm write failed";
        return false;
    }
    writer.finish();
    result->pcm_path = sc.pcm;
    result->audio_channels = channels;
    result->audio_sample_rate = rate;
    result->audio_frames =
        channels ? samples.size() / channels : 0;
    // The still grid covering this audio's length: cover art holds this
    // many frames and the audio curves land on the same grid.
    const double secs =
        rate ? static_cast<double>(result->audio_frames) / rate : 0.0;
    const uint32_t grid = std::max<uint32_t>(
        1, static_cast<uint32_t>(secs * kStillFps) + 1);

    if (!art.empty()) {
        ImageRgba img;
        std::string wic_error;
        if (platform::decode_image_rgba(art.data(), art.size(), &img.width,
                                        &img.height, &img.pixels,
                                        &wic_error)) {
            ImportResult art_result;
            if (write_still_bundle(img, sc, options, grid, &art_result)) {
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

    // Audio curves (bands/onset/bpm) on the still grid - audio files get
    // the same analysis a video's soundtrack gets, so beat clocks and
    // the script analysis ops work on them. With cover art the still
    // bundle wrote brightness/motion/cut at the same grid; merge in.
    {
        mod::AnalysisData adata;
        if (!result->analysis_path.empty())
            mod::load_analysis(result->analysis_path, &adata);
        mod::analyze_audio(samples.data(), result->audio_frames, channels,
                           rate, kStillFps, grid, &adata,
                           progress ? &progress->cancel : nullptr);
        // A cancelled analysis is PARTIAL: persisting it would mark the
        // bundle analyzed with dead curves.
        if (progress && progress->cancel.load()) return false;
        adata.fps = kStillFps;
        adata.frame_count = grid;
        if (mod::write_analysis(sc.analysis, adata))
            result->analysis_path = sc.analysis;
        else
            log_warn("import: audio analysis write failed (non-fatal)");
    }

    if (progress) progress->frames_done.store(1);
    result->ok = true;
    log_info("import: audio %u ch @%u Hz, %llu frames%s", channels, rate,
             static_cast<unsigned long long>(result->audio_frames),
             result->mez_path.empty() ? "" : " + cover art");
    return true;
}

}  // namespace

bool extract_audio_pcm(const std::filesystem::path& source,
                       const std::filesystem::path& dest_pcm,
                       std::string* error) {
    auto fail = [&](std::string what) {
        if (error) *error = std::move(what);
        return false;
    };
    auto write_pcm = [&](const std::vector<int16_t>& samples,
                         uint32_t channels, uint32_t rate) {
        PcmWriter writer;
        if (!writer.open(dest_pcm, channels, rate))
            return fail("cannot create " + path_to_u8(dest_pcm));
        if (!writer.append(samples.data(), samples.size()))
            return fail("pcm write failed");
        writer.finish();
        return true;
    };
    const std::wstring ext = lower_ext(source);

    if (ext == L".wav") {
        WavData wav;
        std::string wav_error;
        if (!read_wav(source, &wav, &wav_error))
            return fail("wav: " + wav_error);
        return write_pcm(wav.samples, wav.channels, wav.sample_rate);
    }
    if (ext == L".mp3") {
        Mp3Data mp3;
        std::string mp3_error;
        if (!read_mp3(source, &mp3, &mp3_error))
            return fail("mp3: " + mp3_error);
        return write_pcm(mp3.samples, mp3.channels, mp3.sample_rate);
    }

    platform::MfSession session;
    if (!session.ok()) return fail("Media Foundation unavailable");
    BmffFile file;
    std::string demux_error;
    if (!file.open(source, &demux_error))
        return fail("demux: " + demux_error);
    const TrackInfo* audio = file.movie().first_audio();
    if (!aac_track(audio)) return fail("no AAC audio track");
    ImportResult scratch;
    if (!import_audio(file, *audio, dest_pcm, &scratch))
        return fail(scratch.error);
    if (scratch.pcm_path.empty()) return fail("audio track decoded empty");
    return true;
}

ImportResult consolidate_video(const std::filesystem::path& source,
                               const std::filesystem::path& dest_dir,
                               ImportProgress* progress) {
    ImportResult result;
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
    if (!video || std::string(video->fourcc) != "avc1" ||
        video->samples.empty()) {
        result.error = "no H.264 video track";
        return result;
    }
    stamp_video_facts(*video, &result);
    const uint32_t w = result.width;
    const uint32_t h = result.height;
    const uint32_t frames = result.frame_count;
    const uint32_t frame_duration = std::max(1u, video->samples[0].duration);
    if (progress) {
        progress->ready.store(true);
        progress->frames_total.store(frames);
    }

    platform::H264Decoder decoder;
    if (!decoder.create(video->avcc, w, h, &error, /*allow_d3d=*/true,
                        /*low_latency=*/false)) {
        result.error = "decoder: " + error;
        return result;
    }

    // Sequential decode feeding the encoder in lockstep; frames come out
    // in presentation order, the same order the native path's frame
    // index serves them. receive() emits tightly packed NV12
    // (stride == width), exactly what the encoder's feed wants — the
    // buffer swaps through untouched.
    const double to_100ns = 1.0e7 / video->timescale;
    size_t next_sample = 0;
    bool drained = false;
    std::vector<uint8_t> sample_bytes;
    platform::VideoFrameNV12 nv12;
    const FrameProducer producer = [&](uint32_t f,
                                       std::vector<uint8_t>& out) -> bool {
        for (;;) {
            if (progress && progress->cancel.load()) return false;
            if (decoder.receive(nv12)) break;
            if (next_sample < video->samples.size()) {
                const SampleInfo& sm = video->samples[next_sample++];
                if (!file.read_sample(sm, sample_bytes)) return false;
                const int64_t pts = static_cast<int64_t>(
                    static_cast<double>(static_cast<int64_t>(sm.dts) +
                                        sm.cts_offset) *
                    to_100ns);
                const int64_t sdur =
                    static_cast<int64_t>(sm.duration * to_100ns);
                if (!decoder.feed(sample_bytes.data(), sample_bytes.size(),
                                  pts, sdur, sm.keyframe))
                    return false;
            } else if (!drained) {
                decoder.drain();
                drained = true;
            } else {
                return false;   // decoder came up short of the count
            }
        }
        out.swap(nv12.data);
        if (progress) progress->frames_done.store(f + 1);
        return true;
    };

    // All-intra needs generous rate to hold quality: ~0.3 bits per pixel
    // per frame, the ballpark of a decent intra-only intermediate.
    ExportOptions options;
    options.gop_frames = 1;
    options.video_bitrate_bps = static_cast<uint32_t>(std::clamp(
        static_cast<double>(w) * h * result.fps * 0.3, 10.0e6, 160.0e6));

    std::filesystem::path out_path =
        sidecars_for_stem(dest_dir, source.stem().wstring()).base;
    out_path.replace_extension(".intra.mp4");
    const ExportResult exported =
        export_movie(w, h, video->timescale, frame_duration, frames,
                     producer, ExportAudio{}, out_path, options, nullptr);
    if (!exported.ok) {
        result.error = (progress && progress->cancel.load())
                           ? "cancelled"
                           : exported.error;
        return result;
    }
    result.ok = true;
    log_info("consolidate: %s -> all-intra %ux%u, %u frames @%0.3f fps",
             path_to_u8(source.filename()).c_str(), w, h, frames,
             result.fps);
    return result;
}

bool rebuild_still_thumbs(const std::filesystem::path& mez_path,
                          const std::filesystem::path& thumbs_path) {
    codec::MezReader reader;
    std::string error;
    if (!reader.open(mez_path, &error)) return false;
    codec::DecodedFrame frame;
    if (!reader.decode(0, frame) || frame.nv12) return false;
    I420Frame f;
    f.width = frame.width;
    f.height = frame.height;
    const uint32_t cw = (f.width + 1) / 2;
    const uint32_t ch = (f.height + 1) / 2;
    f.y.resize(static_cast<size_t>(f.width) * f.height);
    f.u.resize(static_cast<size_t>(cw) * ch);
    f.v.resize(static_cast<size_t>(cw) * ch);
    for (uint32_t y = 0; y < f.height; ++y)
        std::memcpy(f.y.data() + static_cast<size_t>(y) * f.width,
                    frame.y.data() + static_cast<size_t>(y) * frame.y_stride,
                    f.width);
    for (uint32_t y = 0; y < ch; ++y) {
        std::memcpy(f.u.data() + static_cast<size_t>(y) * cw,
                    frame.u.data() + static_cast<size_t>(y) * frame.uv_stride,
                    cw);
        std::memcpy(f.v.data() + static_cast<size_t>(y) * cw,
                    frame.v.data() + static_cast<size_t>(y) * frame.uv_stride,
                    cw);
    }
    ThumbStrip strip;
    strip.add(f);
    return strip.write(thumbs_path);
}

ImportResult resume_video_pass(const std::filesystem::path& source,
                               const std::filesystem::path& dest_dir,
                               const ImportOptions& options,
                               ImportProgress* progress) {
    // Names the background thread's work: a wedge in here otherwise
    // reads as a silent hang at close (the join in ~ImportJob).
    log_info("ingest: resuming video pass %s",
             path_to_u8(source).c_str());
    ImportResult result;
    platform::MfSession session;
    if (!session.ok()) {
        result.error = "Media Foundation unavailable";
        return result;
    }
    log_info("ingest: mf session up");
    BmffFile file;
    std::string error;
    if (!file.open(source, &error)) {
        result.error = "demux: " + error;
        return result;
    }
    log_info("ingest: demux open");
    const TrackInfo* video = file.movie().first_video();
    if (!video || std::string(video->fourcc) != "avc1" ||
        video->samples.empty()) {
        result.error = "no H.264 video track";
        return result;
    }
    stamp_video_facts(*video, &result);

    // The fast-stage sidecar carries the audio curves; the pass rewrites
    // it with the video curves merged, exactly as first ingest would
    // have. A missing/unreadable sidecar still gets the video curves.
    const std::wstring stem = source.stem().wstring();
    const SidecarPaths sc = sidecars_for_stem(dest_dir, stem);
    mod::AnalysisData analysis;
    mod::load_analysis(sc.analysis, &analysis);
    analysis.fps = result.fps;
    analysis.frame_count = result.frame_count;

    if (progress) progress->ready.store(true);
    result.ok = ingest_video_pass(file, *video, options, progress, &analysis,
                                  dest_dir, stem, &result);
    if (!result.ok && result.error.empty())
        result.error = "video pass failed";
    return result;
}

ImportResult import_media(const std::filesystem::path& source,
                          const std::filesystem::path& dest_dir,
                          const ImportOptions& options,
                          ImportProgress* progress) {
    ImportResult result;

    // Still images (PNG/TGA) skip Media Foundation entirely — the in-repo
    // decoders and the mezzanine writer are all it takes.
    const std::wstring ext = lower_ext(source);
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

    // ---- fast stage: everything the asset needs to be USABLE. Video
    // facts come from the container (playback decodes the source in
    // place - no transcode); the AAC decode into the PCM sidecar and the
    // audio curves are the only real work. `ready` flips here and the
    // app binds/places the asset while the video pass below still runs.
    stamp_video_facts(*video, &result);

    const SidecarPaths sc = sidecars_for_stem(dest_dir, stem);
    const TrackInfo* audio = file.movie().first_audio();
    if (aac_track(audio) &&
        !import_audio(file, *audio, sc.pcm, &result))
        return result;

    mod::AnalysisData analysis;
    analysis.fps = result.fps;
    analysis.frame_count = result.frame_count;
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
                               result.fps, result.frame_count, &analysis,
                               progress ? &progress->cancel : nullptr);
        }
    }
    if (progress && progress->cancel.load()) {
        result.error = "cancelled";
        return result;
    }
    // The analysis sidecar is the READY marker resolve_bundle gates on:
    // written last in the fast stage, deleted never, rewritten (with the
    // video curves merged in) when the pass below completes.
    if (mod::write_analysis(sc.analysis, analysis))
        result.analysis_path = sc.analysis;
    else
        log_warn("ingest: analysis write failed");

    result.ok = true;
    if (progress && !result.pcm_path.empty())
        progress->pcm = load_pcm(result.pcm_path);
    if (progress) progress->ready.store(true);
    log_info("ingest: %s ready — %u frames @%0.3f fps%s",
             path_to_u8(source).c_str(), result.frame_count, result.fps,
             result.pcm_path.empty() ? "" : " + audio");

    // ---- background stage, same job: the one full decode feeding the
    // video curves and the thumbnail strip.
    if (!(progress && progress->cancel.load()))
        ingest_video_pass(file, *video, options, progress, &analysis,
                          dest_dir, stem, &result);
    return result;
}

}  // namespace looks::media
