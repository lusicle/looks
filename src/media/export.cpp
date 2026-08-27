#include "media/export.h"

#include <algorithm>

#include "media/bmff_mux.h"
#include "media/h264_util.h"
#include "media/pcm.h"
#include "media/sample_clock.h"
#include "platform/win/mf_codec.h"
#include "util/log.h"

namespace looks::media {

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

}  // namespace

namespace {

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

    // ---- video: produce -> encode -> AVCC packets (pts == dts, no B).
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

    // ---- audio: the project mix -> AAC packets.
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
                audio_packets.push_back({packet.data, packet.pts_100ns});
        };
        // Export exactly the video's duration of audio, measured by the
        // same frame->sample rule the monitor cursor uses. The skip maps
        // output sample s to mix position s + skip; anything outside
        // every source is silence (a gap, a negative nudge, trim past
        // the end).
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

    // ---- mux (faststart)
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

// Every failure exits through here so the log always carries the reason,
// not just the status line.
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
