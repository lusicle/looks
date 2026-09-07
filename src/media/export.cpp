#include "media/export.h"
#include "platform/win/wic_image.h"
#include <cmath>

#include <algorithm>

#include "media/bmff_mux.h"
#include "media/h264_util.h"
#include "media/pcm.h"
#include "media/sample_clock.h"
#include "platform/win/mf_codec.h"
#include "util/log.h"

namespace looks::media {

const char* render_format_name(RenderFormat format) {
    switch (format) {
        case RenderFormat::Mp4: return "mp4";
        case RenderFormat::Gif: return "gif";
        case RenderFormat::PngFrame: return "png";
        case RenderFormat::PngSequence: return "png_sequence";
    }
    return "";
}

json::Value render_settings_json(const RenderSettings& s) {
    auto v = json::Value::make_object();
    v.set("format", render_format_name(s.format));
    v.set("width", int64_t(s.width));
    v.set("height", int64_t(s.height));
    v.set("fps", s.fps);
    v.set("bitrate", double(s.bitrate_mbps));
    v.set("audio", s.audio);
    v.set("alpha", s.alpha);
    v.set("colors", int64_t(s.gif_colors));
    v.set("dither", s.gif_dither);
    v.set("loops", int64_t(s.gif_loops));
    v.set("alpha_threshold", double(s.gif_alpha_threshold));
    return v;
}

bool read_render_settings(const json::Value& v, RenderSettings& s, std::string& error) {
    error.clear();
    if (!v.is_object()) { error = "render settings must be an object"; return false; }
    RenderSettings next = s;
    const std::string name = v.find("format") ? v.get("format").as_string() : render_format_name(s.format);
    bool found = false;
    for (int i = 0; i < 4; ++i)
        if (name == render_format_name(RenderFormat(i))) { next.format = RenderFormat(i); found = true; }
    if (!found) { error = "format must be mp4, gif, png, or png_sequence"; return false; }
    auto number = [&](const char* key, double fallback, double lo, double hi, bool integer = false) {
        if (v.find(key) && !v.get(key).is_number()) error = std::string("expected a number for ") + key;
        const double n = v.get(key).as_number(fallback);
        if (!std::isfinite(n) || n < lo || n > hi || (integer && std::floor(n) != n))
            error = std::string("invalid render setting: ") + key;
        return std::clamp(std::isfinite(n) ? n : fallback, lo, hi);
    };
    next.width = uint32_t(number("width", s.width, 0, UINT32_MAX, true));
    next.height = uint32_t(number("height", s.height, 0, UINT32_MAX, true));
    if (bool(next.width) != bool(next.height)) error = "set both width and height, or zero for source size";
    next.fps = number("fps", s.fps, 0, 1000);
    next.bitrate_mbps = float(number("bitrate", s.bitrate_mbps, 1, 60));
    next.gif_colors = uint32_t(number("colors", s.gif_colors, 2, 256, true));
    next.gif_loops = uint32_t(number("loops", s.gif_loops, 0, 1, true));
    next.gif_alpha_threshold = float(number("alpha_threshold", s.gif_alpha_threshold, 0, 100));
    for (const char* key : {"audio", "alpha", "dither"})
        if (v.find(key) && !v.get(key).is_bool()) error = std::string("expected a boolean for ") + key;
    next.audio = v.get("audio").as_bool(s.audio);
    next.alpha = v.get("alpha").as_bool(s.alpha);
    next.gif_dither = v.get("dither").as_bool(s.gif_dither);
    if (!error.empty()) return false;
    s = next;
    return true;
}

std::filesystem::path image_output_path(const std::filesystem::path& path,
                                        RenderFormat format, uint32_t index) {
    if (format != RenderFormat::PngSequence) return path;
    wchar_t suffix[32];
    std::swprintf(suffix, std::size(suffix), L"_%06u.png", index + 1);
    return path.parent_path() / (path.stem().wstring() + suffix);
}

ExportResult export_images(uint32_t width, uint32_t height, uint32_t fps_num,
                           uint32_t fps_den, uint32_t frame_count,
                           const FrameProducer& producer, const std::filesystem::path& path,
                           const RenderSettings& settings, bool overwrite,
                           ExportProgress* progress) {
    auto fail = [](std::string error) { return ExportResult{false, std::move(error)}; };
    if (!width || !height || !fps_num || !fps_den || !frame_count)
        return fail("invalid image export dimensions, rate, or range");
    if (settings.format == RenderFormat::Mp4 ||
        (settings.format == RenderFormat::PngFrame && frame_count != 1) ||
        uint64_t(width) * height > UINT32_MAX / 4)
        return fail("invalid image export format or size");
    if (settings.format == RenderFormat::Gif && double(fps_num) / fps_den > 100.0)
        return fail("GIF frame rate must be at most 100 fps");
    const uint32_t files = settings.format == RenderFormat::PngSequence ? frame_count : 1;
    std::error_code ec;
    for (uint32_t i = 0; i < files; ++i) {
        if (progress && progress->cancel.load()) return fail("cancelled");
        const bool exists = std::filesystem::exists(image_output_path(path, settings.format, i), ec);
        if (ec) return fail("cannot inspect output location: " + ec.message());
        if (exists && !overwrite) return fail("output exists; enable overwrite or choose another name");
    }
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return fail("cannot create output directory: " + ec.message());
    }
    if (progress) {
        progress->frames_done.store(0);
        progress->frames_total.store(frame_count);
    }
    platform::GifEncoder gif;
    std::string error;
    if (settings.format == RenderFormat::Gif &&
        !gif.open(path, width, height, settings.gif_loops, &error)) return fail(error);
    std::vector<uint8_t> rgba;
    for (uint32_t i = 0; i < frame_count; ++i) {
        if (progress && progress->cancel.load()) return fail("cancelled");
        if (!producer(i, rgba) || rgba.size() != size_t(width) * height * 4)
            return fail(progress && progress->cancel.load() ? "cancelled" : "image render failed");
        if (settings.format == RenderFormat::Gif) {
            const uint64_t start = uint64_t(std::llround(double(i) * 100 * fps_den / fps_num));
            const uint64_t end = uint64_t(std::llround(double(i + 1) * 100 * fps_den / fps_num));
            if (end <= start || end - start > 65535) return fail("GIF delay is out of range");
            if (!gif.add(rgba.data(), uint32_t(end - start), settings.gif_colors,
                         settings.gif_dither, settings.alpha, settings.gif_alpha_threshold, &error))
                return fail(error);
        } else if (!platform::encode_png(image_output_path(path, settings.format, i),
                                          rgba.data(), width, height, &error)) return fail(error);
        if (progress) progress->frames_done.store(i + 1);
    }
    if (settings.format == RenderFormat::Gif && !gif.finish(&error)) return fail(error);
    return {true, {}};
}

namespace {

struct VideoPacket {
    std::vector<uint8_t> avcc_sample;
    int64_t pts;
    bool keyframe;
};

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t pts;
};

ExportResult export_movie_impl(uint32_t width, uint32_t height,
                               uint32_t fps_num, uint32_t fps_den,
                               uint32_t frame_count,
                               const FrameProducer& producer,
                               const ExportAudio& audio,
                               const std::filesystem::path& out_mp4,
                               const ExportOptions& options,
                               ExportProgress* progress) {
    ExportResult result;
    if (width == 0 || height == 0 || frame_count == 0 || fps_num == 0 ||
        fps_den == 0) {
        result.error = "invalid export parameters";
        return result;
    }
    if (progress) {
        progress->frames_done = 0;
        progress->frames_total = frame_count;
    }

    platform::MfSession session;
    if (!session.ok()) {
        result.error = "Media Foundation unavailable";
        return result;
    }

    // The encoder emits no B-frames, so pts == dts.
    platform::H264Encoder video_encoder;
    if (!video_encoder.create(width, height, fps_num, fps_den,
                              options.video_bitrate_bps, &result.error,
                              options.gop_frames))
        return result;

    std::vector<VideoPacket> video_packets;
    std::vector<uint8_t> sps, pps;
    platform::EncodedPacket packet;
    auto pump_video = [&]() {
        while (video_encoder.receive(packet)) {
            VideoPacket p;
            const bool idr = annexb_to_avcc_sample(
                packet.data.data(), packet.data.size(), p.avcc_sample, &sps,
                &pps);
            p.pts = packet.pts_100ns;
            p.keyframe = idr || packet.keyframe;
            if (!p.avcc_sample.empty()) video_packets.push_back(std::move(p));
        }
    };

    std::vector<uint8_t> nv12;
    for (uint32_t f = 0; f < frame_count; ++f) {
        if (progress && progress->cancel.load()) {
            result.error = "cancelled";
            return result;
        }
        if (!producer(f, nv12)) {
            result.error = "frame render failed at " + std::to_string(f);
            return result;
        }
        if (nv12.size() < static_cast<size_t>(width) * height * 3 / 2) {
            result.error = "producer returned short frame";
            return result;
        }
        const int64_t pts = 10'000'000ll * f * fps_den / fps_num;
        const int64_t next = 10'000'000ll * (f + 1) * fps_den / fps_num;
        if (!video_encoder.feed_nv12(nv12.data(), pts, next - pts)) {
            result.error = "video encode failed at " + std::to_string(f);
            return result;
        }
        pump_video();
        if (progress) progress->frames_done = f + 1;
    }
    video_encoder.drain();
    pump_video();
    if (video_packets.empty() || sps.empty() || pps.empty()) {
        result.error = "encoder produced no packets";
        return result;
    }

    std::vector<AudioPacket> audio_packets;
    std::vector<uint8_t> audio_asc;
    uint32_t audio_channels = 0;
    uint32_t audio_rate = 0;
    if (audio.channels > 0 && audio.rate > 0 && audio.fill) {
        audio_channels = audio.channels;
        audio_rate = audio.rate;

        platform::AacEncoder audio_encoder;
        if (!audio_encoder.create(audio_channels, audio_rate,
                                  options.audio_bitrate_bps, &result.error))
            return result;
        audio_asc = audio_encoder.audio_specific_config();

        auto pump_audio = [&]() {
            while (audio_encoder.receive(packet))
                audio_packets.push_back(
                    {std::move(packet.data), packet.pts_100ns});
        };
        // Use the same frame-to-sample rule as the monitor; sample s
        // reads mix position s + skip.
        const double vfps =
            static_cast<double>(fps_num) / static_cast<double>(fps_den);
        const uint64_t total_frames = static_cast<uint64_t>(
            frame_to_sample(frame_count, vfps, audio_rate));
        const int64_t skip = options.audio_skip_samples;
        std::vector<int16_t> chunk(static_cast<size_t>(1024) * audio_channels);
        for (uint64_t s = 0; s < total_frames; s += 1024) {
            if (progress && progress->cancel.load()) {
                result.error = "cancelled";
                return result;
            }
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(1024, total_frames - s));
            audio.fill(static_cast<int64_t>(s) + skip, chunk.data(),
                       static_cast<uint32_t>(n));
            const int64_t pts =
                static_cast<int64_t>(s) * 10'000'000ll / audio_rate;
            if (!audio_encoder.feed(chunk.data(), n * audio_channels, pts)) {
                result.error = "audio encode failed";
                return result;
            }
            pump_audio();
        }
        audio_encoder.drain();
        pump_audio();
    }

    MuxVideoParams video_params;
    video_params.width = width;
    video_params.height = height;
    video_params.timescale = 90000;
    video_params.avcc = build_avcc(sps, pps);

    MuxAudioParams audio_params;
    const bool has_audio = !audio_packets.empty();
    if (has_audio) {
        audio_params.channels = audio_channels;
        audio_params.sample_rate = audio_rate;
        audio_params.avg_bitrate = options.audio_bitrate_bps;
        audio_params.audio_specific_config = audio_asc;
    }

    BmffMuxer muxer;
    if (!muxer.open(out_mp4, video_params, has_audio ? &audio_params : nullptr)) {
        result.error = "mux open failed";
        return result;
    }
    const uint32_t video_tick =
        static_cast<uint32_t>(90000ull * fps_den / fps_num);
    for (const VideoPacket& p : video_packets) {
        // 100ns -> 90 kHz ticks: * 90000 / 1e7 reduced.
        const uint64_t dts = static_cast<uint64_t>(p.pts) * 9 / 1000;
        if (!muxer.add_video_sample(p.avcc_sample.data(), p.avcc_sample.size(),
                                    dts, video_tick, 0, p.keyframe)) {
            result.error = "mux video failed";
            return result;
        }
    }
    for (const AudioPacket& p : audio_packets) {
        const uint64_t dts =
            static_cast<uint64_t>(p.pts) * audio_rate / 10'000'000ll;
        if (!muxer.add_audio_sample(p.data.data(), p.data.size(), dts, 1024)) {
            result.error = "mux audio failed";
            return result;
        }
    }
    if (!muxer.finish()) {
        result.error = "mux finish failed";
        return result;
    }

    log_info("export: %s  %u frames %ux%u%s", out_mp4.string().c_str(),
             frame_count, width, height, has_audio ? " + audio" : "");
    result.ok = true;
    return result;
}

}  // namespace

// Every failure exits through here so the log carries the reason.
ExportResult export_movie(uint32_t width, uint32_t height, uint32_t fps_num,
                          uint32_t fps_den, uint32_t frame_count,
                          const FrameProducer& producer,
                          const ExportAudio& audio,
                          const std::filesystem::path& out_mp4,
                          const ExportOptions& options,
                          ExportProgress* progress) {
    ExportResult result =
        export_movie_impl(width, height, fps_num, fps_den, frame_count,
                          producer, audio, out_mp4, options, progress);
    if (!result.ok)
        log_warn("export failed: %s (%s)", result.error.c_str(),
                 out_mp4.string().c_str());
    return result;
}

}  // namespace looks::media
