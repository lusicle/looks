// Dev CLI: generates a real MP4 (H.264 + AAC) from synthetic content
// through media::export_movie - the SAME driver the app's export uses,
// so a fixture exercises exactly the shipped encode->mux path.
//
//   looks_makefixture <out.mp4> [seconds] [width] [height] [gop_frames]
//
// gop_frames > 0 pins the keyframe interval - seek-latency benches need
// fixtures with controlled spacing.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/dev_synth.h"
#include "media/export.h"
#include "media/player.h"

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
    const uint32_t video_frames = static_cast<uint32_t>(seconds * fps + 0.5);

    looks::media::ExportOptions options;
    options.video_bitrate_bps = 1'500'000;
    options.gop_frames = gop_frames;

    auto producer = [&](uint32_t frame, std::vector<uint8_t>& nv12) {
        nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
        uint8_t* y = nv12.data();
        for (uint32_t r = 0; r < height; ++r)
            for (uint32_t c = 0; c < width; ++c)
                y[r * width + c] = looks::devsynth::luma(c, r, frame, width);
        uint8_t* uv = nv12.data() + static_cast<size_t>(width) * height;
        for (uint32_t r = 0; r < height / 2; ++r)
            for (uint32_t c = 0; c < width / 2; ++c) {
                uv[r * width + c * 2] = looks::devsynth::cb(c, frame);
                uv[r * width + c * 2 + 1] = looks::devsynth::cr(r, frame);
            }
        return true;
    };

    // 440 Hz sine on both channels, a pure function of sample position.
    looks::media::ExportAudio audio;
    audio.channels = looks::media::Player::kChannels;
    audio.rate = looks::media::Player::kClockRate;
    audio.fill = [&audio](int64_t first, int16_t* out, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i) {
            const double t =
                static_cast<double>(first + static_cast<int64_t>(i)) /
                audio.rate;
            const int16_t v = static_cast<int16_t>(
                std::sin(t * 2.0 * 3.14159265358979 * 440.0) * 0.4 *
                32767.0);
            for (uint32_t c = 0; c < audio.channels; ++c)
                out[i * audio.channels + c] = v;
        }
    };

    const looks::media::ExportResult result = looks::media::export_movie(
        width, height, fps, 1, video_frames, producer, audio, argv[1],
        options);
    if (!result.ok) {
        std::fprintf(stderr, "makefixture: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("%u frames %ux%u + audio -> ok\n", video_frames, width,
                height);
    return 0;
}
