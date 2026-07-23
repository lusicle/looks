// Dev CLI: headless player verification. Opens an imported bundle, plays
// with the audio clock running, samples the playhead, exercises seek and
// trim, and reports pass/fail.
//   looks_playtest <bundle.mez> [bundle.pcm]

#include <windows.h>

#include <cstdio>
#include <string>

#include "media/player.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: looks_playtest <bundle.mez> [bundle.pcm]\n");
        return 2;
    }
    looks::media::Player player;
    std::string error;
    if (!player.open(argv[1], argc > 2 ? argv[2] : L"", &error)) {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("open: %u frames @ %.3f fps, %.3fs, audio %u ch %u Hz\n",
                player.frame_count(), player.fps(), player.duration_seconds(),
                player.audio_channels(), player.audio_sample_rate());

    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };

    // Decode-ahead fills without playback.
    Sleep(200);
    auto frame = player.current_frame();
    expect(frame != nullptr, "frame available while paused");
    if (frame) expect(frame->width > 0, "decoded frame has pixels");
    expect(player.current_frame_index() == 0, "playhead at 0 before play");

    // Play ~1.1s of a looping clip; the clock must advance.
    player.set_looping(true);
    player.play();
    Sleep(1100);
    const double pos = player.position_seconds();
    const uint32_t idx = player.current_frame_index();
    std::printf("after 1.1s: pos=%.3fs frame=%u playing=%d\n", pos, idx,
                player.playing());
    expect(pos > 0.8 && pos < 1.6, "audio clock advanced ~1.1s");
    expect(idx > 20, "video chased the clock");
    frame = player.current_frame();
    expect(frame != nullptr, "frame available during playback");
    player.pause();
    Sleep(60);
    const double paused_pos = player.position_seconds();
    Sleep(120);
    expect(player.position_seconds() == paused_pos, "clock frozen while paused");

    // Instant seek.
    player.seek_frame(5);
    expect(player.current_frame_index() == 5, "seek lands on frame 5");
    Sleep(150);
    frame = player.current_frame();
    expect(frame != nullptr, "frame decoded after seek");

    // Trim + loop stays inside the region.
    player.set_trim(10, 20);
    player.seek_frame(10);
    player.play();
    Sleep(700);   // > 10 frames at 30fps -> must have wrapped
    const uint32_t trimmed_idx = player.current_frame_index();
    std::printf("trim [10,20): frame=%u\n", trimmed_idx);
    expect(trimmed_idx >= 10 && trimmed_idx < 20, "loop stays inside trim");
    player.pause();

    std::printf(failures == 0 ? "PASS\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
