#include <windows.h>

#include <crtdbg.h>
#include <cstdio>
#include <string>

#include "codec/mez.h"
#include "doc/document.h"
#include "doc/layer_commands.h"
#include "doc/look_commands.h"
#include "media/audio_mix.h"
#include "media/decode_pool.h"
#include "media/player.h"

int wmain(int argc, wchar_t** argv) {
    // A crash must report, not raise a modal box that stalls the runner.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: looks_playtest <bundle.mez> [bundle.pcm]\n");
        return 2;
    }
    using namespace looks;

    codec::MezReader probe;
    std::string error;
    if (!probe.open(argv[1], &error)) {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        return 1;
    }

    doc::Document doc;
    {
        doc::Look seed = doc::make_look(doc, "look 1");
        seed.sources.push_back(
            doc::make_source(doc, doc::SourceKind::Media));
        seed.links = {{seed.sources[0].id, 0, 0}};
        doc.looks.push_back(std::move(seed));
    }
    doc::Asset asset;
    asset.id = doc.next_effect_id++;
    asset.name = "media";
    asset.frame_count = probe.frame_count();
    asset.fps = probe.fps();
    asset.width = probe.width();
    asset.height = probe.height();
    doc.assets.push_back(asset);
    doc.looks[0].sources[0].asset = asset.id;
    {
        doc::Placement block;
        block.id = doc.next_effect_id++;
        block.target = doc.looks[0].id;
        doc.root().tracks[0].placements.push_back(block);
    }
    doc.fps = asset.fps > 0.0 ? asset.fps : 30.0;

    media::AssetBundle bundle;
    bundle.asset = asset.id;
    bundle.mez = argv[1];
    if (argc > 2) bundle.pcm = argv[2];
    bundle.frames = asset.frame_count;
    bundle.width = asset.width;
    bundle.height = asset.height;
    bundle.fps = asset.fps;
    const std::vector<media::AssetBundle> bundles{bundle};

    const uint32_t span = doc::sequence_duration(doc, doc.root());
    media::Player player;
    player.configure(doc.fps, span);
    std::printf("timeline: %u frames @ %.3f fps, %.3fs, monitor %u ch %u Hz\n",
                player.frame_count(), player.fps(), player.duration_seconds(),
                player.audio_channels(), player.audio_sample_rate());

    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };
    expect(span == probe.frame_count(),
           "timeline length derives from the placement");

    {
        auto mix = std::make_shared<media::MixState>();
        mix->fps = doc.fps;
        mix->rate = player.audio_sample_rate();
        mix->channels = player.audio_channels();
        if (auto pcm = media::load_pcm(bundle.pcm)) {
            media::MixNode leaf;
            leaf.pcm = pcm;
            mix->nodes.push_back(std::move(leaf));
            media::MixNode hop;
            hop.windowed = true;
            hop.w1 = span;
            hop.inputs.push_back(0);
            mix->nodes.push_back(std::move(hop));
            mix->root = 1;
            media::prepare_mix(*mix);
        }
        player.set_mix(std::move(mix));
    }

    media::DecodePool pool;
    pool.set_document(doc, doc.root_sequence, bundles, 1);
    const auto& first = pool.collect(0);
    expect(first.size() == 1, "one placement decodes at frame 0");
    expect(!first.empty() && first[0].frame &&
               first[0].frame->width == probe.width(),
           "decoded frame has pixels");
    expect(player.current_frame_index() == 0, "playhead at 0 before play");

    player.set_looping(true);
    player.play();
    Sleep(1100);
    const double pos = player.position_seconds();
    const uint32_t idx = player.current_frame_index();
    std::printf("after 1.1s: pos=%.3fs frame=%u playing=%d\n", pos, idx,
                player.playing());
    expect(pos > 0.8 && pos < 1.6, "clock advanced ~1.1s");
    expect(idx > 20, "playhead chased the clock");
    expect(!pool.collect(idx).empty(), "pool serves the playhead frame");
    player.pause();
    Sleep(60);
    const double paused_pos = player.position_seconds();
    Sleep(120);
    expect(player.position_seconds() == paused_pos,
           "clock frozen while paused");

    player.seek_frame(5);
    expect(player.current_frame_index() == 5, "seek lands on frame 5");
    const auto& sought = pool.collect(5);
    expect(sought.size() == 1 && sought[0].frame != nullptr,
           "frame decoded after seek");

    const uint64_t before = pool.misses();
    for (uint32_t f = 5; f < 9; ++f) pool.collect(f);
    const uint64_t walked = pool.misses() - before;
    std::printf("prewarm: %llu disk decodes over 4 sequential frames\n",
                static_cast<unsigned long long>(walked));

    player.set_trim(10, 20);
    player.seek_frame(10);
    player.play();
    Sleep(700);   // more than 10 frames at 30 fps: the loop must wrap
    const uint32_t trimmed_idx = player.current_frame_index();
    std::printf("trim [10,20): frame=%u\n", trimmed_idx);
    expect(trimmed_idx >= 10 && trimmed_idx < 20, "loop stays inside trim");
    player.pause();

    std::printf(failures == 0 ? "PASS\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
