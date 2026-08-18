// Dev CLI: generates a real MP4 (H.264 + AAC) from synthetic content using
// our encoder MFT glue + BMFF muxer. Primary use: hermetic end-to-end
// verification of the media pipeline (makefixture -> looks_import), and
// later the determinism harness. Also doubles as the first exercise of the
// export path's building blocks.
//
//   looks_makefixture <out.mp4> [seconds] [width] [height] [gop_frames]
//
// gop_frames > 0 pins the keyframe interval - seek-latency benches need
// fixtures with controlled spacing.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "media/bmff_mux.h"
#include "media/h264_util.h"
#include "platform/win/mf_codec.h"

namespace {

// Moving diagonal gradient + a sweeping bright bar; NV12, packed.
void synth_frame(std::vector<uint8_t>& nv12, uint32_t w, uint32_t h,
                 uint32_t frame) {
    nv12.resize(static_cast<size_t>(w) * h * 3 / 2);
    uint8_t* y = nv12.data();
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t c = 0; c < w; ++c) {
            uint32_t v = (c + r + frame * 3) & 0xFF;
            const uint32_t bar = (frame * 5) % w;
            if (c >= bar && c < bar + 24) v = 235;
            y[r * w + c] = static_cast<uint8_t>(16 + (v * 219) / 255);
        }
    }
    uint8_t* uv = nv12.data() + static_cast<size_t>(w) * h;
    for (uint32_t r = 0; r < h / 2; ++r) {
        for (uint32_t c = 0; c < w / 2; ++c) {
            uv[r * w + c * 2] = static_cast<uint8_t>(96 + ((c * 2 + frame) & 63));
            uv[r * w + c * 2 + 1] = static_cast<uint8_t>(160 - ((r + frame) & 63));
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: looks_makefixture <out.mp4> [seconds] [w] [h] "
                     "[gop_frames]\n");
        return 2;
    }
    const double seconds = argc > 2 ? _wtof(argv[2]) : 2.0;
    const uint32_t width = argc > 3 ? static_cast<uint32_t>(_wtoi(argv[3])) : 320;
    const uint32_t height = argc > 4 ? static_cast<uint32_t>(_wtoi(argv[4])) : 240;
    const uint32_t gop_frames =
        argc > 5 ? static_cast<uint32_t>(_wtoi(argv[5])) : 0;
    const uint32_t fps = 30;
    const uint32_t sample_rate = 48000;
    const uint32_t channels = 2;
    const uint32_t video_frames = static_cast<uint32_t>(seconds * fps + 0.5);

    looks::platform::MfSession session;
    if (!session.ok()) {
        std::fprintf(stderr, "Media Foundation unavailable\n");
        return 1;
    }

    std::string error;
    looks::platform::H264Encoder video_encoder;
    if (!video_encoder.create(width, height, fps, 1, 1'500'000, &error,
                              gop_frames)) {
        std::fprintf(stderr, "video encoder: %s\n", error.c_str());
        return 1;
    }
    looks::platform::AacEncoder audio_encoder;
    if (!audio_encoder.create(channels, sample_rate, 128000, &error)) {
        std::fprintf(stderr, "audio encoder: %s\n", error.c_str());
        return 1;
    }

    // ---- encode everything up front (fixture-sized content).
    struct VideoPacket {
        std::vector<uint8_t> avcc_sample;
        int64_t pts;
        bool keyframe;
    };
    std::vector<VideoPacket> video_packets;
    std::vector<uint8_t> sps, pps;

    std::vector<uint8_t> nv12;
    looks::platform::EncodedPacket packet;
    const int64_t frame_dur = 10'000'000ll / fps;

    auto pump_video = [&]() {
        while (video_encoder.receive(packet)) {
            VideoPacket p;
            const bool idr = looks::media::annexb_to_avcc_sample(
                packet.data.data(), packet.data.size(), p.avcc_sample, &sps, &pps);
            p.pts = packet.pts_100ns;
            p.keyframe = idr || packet.keyframe;
            if (!p.avcc_sample.empty()) video_packets.push_back(std::move(p));
        }
    };
    for (uint32_t f = 0; f < video_frames; ++f) {
        synth_frame(nv12, width, height, f);
        if (!video_encoder.feed_nv12(nv12.data(), f * frame_dur, frame_dur)) {
            std::fprintf(stderr, "video encode failed at frame %u\n", f);
            return 1;
        }
        pump_video();
    }
    video_encoder.drain();
    pump_video();
    if (video_packets.empty() || sps.empty() || pps.empty()) {
        std::fprintf(stderr, "no video packets / parameter sets\n");
        return 1;
    }

    struct AudioPacket {
        std::vector<uint8_t> data;
        int64_t pts;
    };
    std::vector<AudioPacket> audio_packets;
    auto pump_audio = [&]() {
        while (audio_encoder.receive(packet))
            audio_packets.push_back({packet.data, packet.pts_100ns});
    };
    const uint32_t total_samples = static_cast<uint32_t>(seconds * sample_rate);
    std::vector<int16_t> pcm(static_cast<size_t>(1024) * channels);
    for (uint32_t s = 0; s < total_samples; s += 1024) {
        const uint32_t n = std::min(1024u, total_samples - s);
        for (uint32_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(s + i) / sample_rate;
            const double v = std::sin(t * 2.0 * 3.14159265358979 * 440.0) * 0.4;
            const int16_t sample_value = static_cast<int16_t>(v * 32767.0);
            for (uint32_t c = 0; c < channels; ++c)
                pcm[i * channels + c] = sample_value;
        }
        const int64_t pts = static_cast<int64_t>(s) * 10'000'000ll / sample_rate;
        if (!audio_encoder.feed(pcm.data(), static_cast<size_t>(n) * channels, pts)) {
            std::fprintf(stderr, "audio encode failed\n");
            return 1;
        }
        pump_audio();
    }
    audio_encoder.drain();
    pump_audio();

    // ---- mux
    looks::media::MuxVideoParams video_params;
    video_params.width = width;
    video_params.height = height;
    video_params.timescale = 90000;
    video_params.avcc = looks::media::build_avcc(sps, pps);

    looks::media::MuxAudioParams audio_params;
    audio_params.channels = channels;
    audio_params.sample_rate = sample_rate;
    audio_params.audio_specific_config = audio_encoder.audio_specific_config();

    looks::media::BmffMuxer muxer;
    if (!muxer.open(argv[1], video_params, &audio_params)) {
        std::fprintf(stderr, "muxer open failed\n");
        return 1;
    }
    const uint32_t video_tick = 90000 / fps;
    for (size_t i = 0; i < video_packets.size(); ++i) {
        // pts == dts (no B-frames); rescale 100ns -> 90kHz.
        const uint64_t dts =
            static_cast<uint64_t>(video_packets[i].pts) * 9 / 1000;
        if (!muxer.add_video_sample(video_packets[i].avcc_sample.data(),
                                    video_packets[i].avcc_sample.size(), dts,
                                    video_tick, 0, video_packets[i].keyframe)) {
            std::fprintf(stderr, "mux video failed\n");
            return 1;
        }
    }
    for (const AudioPacket& p : audio_packets) {
        const uint64_t dts =
            static_cast<uint64_t>(p.pts) * sample_rate / 10'000'000ll;
        if (!muxer.add_audio_sample(p.data.data(), p.data.size(), dts, 1024)) {
            std::fprintf(stderr, "mux audio failed\n");
            return 1;
        }
    }
    if (!muxer.finish()) {
        std::fprintf(stderr, "mux finish failed\n");
        return 1;
    }
    std::printf("%u video packets, %zu audio packets -> ok\n",
                static_cast<uint32_t>(video_packets.size()),
                audio_packets.size());
    return 0;
}
