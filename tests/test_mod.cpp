#include <cmath>

#include "doc/effects.h"
#include "doc/mask_commands.h"
#include "doc/mod_commands.h"
#include "doc/stack_commands.h"
#include "mod/analysis.h"
#include "mod/eval.h"
#include "mod/fft.h"
#include "mod/param_table.h"
#include "test_framework.h"

using namespace looks;

namespace {

doc::Document make_doc() {
    doc::Document d;
    d.layers[0].stack.push_back(doc::make_effect(d, doc::EffectType::Vignette));
    d.layers[0].stack.push_back(doc::make_effect(d, doc::EffectType::RgbSplit));
    return d;
}

bool near(float a, float b, float eps = 1.0e-4f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

TEST(mod_param_table_paths) {
    doc::Document d = make_doc();
    auto table = mod::build_param_table(d);
    // global.morph + global.speed + vignette (wet, opacity, amount,
    // radius, softness = 5) + rgb split (4).
    CHECK_EQ(table.size(), size_t{11});
    CHECK_EQ(table[0].path, "global.morph");
    CHECK_EQ(table[0].key.effect_id, uint64_t{0});
    CHECK_EQ(table[1].path, "global.speed");
    CHECK_EQ(table[1].key.param_index, 1);
    CHECK_EQ(table[2].path, "layer0.fx0.wet");
    CHECK_EQ(table[4].path, "layer0.fx0.amount");
    CHECK_EQ(table[7].path, "layer0.fx1.wet");
    CHECK_EQ(table[9].path, "layer0.fx1.shift_x");
    CHECK_EQ(table[9].key.effect_id, d.layers[0].stack[1].id);
    CHECK_EQ(table[9].min_value, -64.0f);
    CHECK_EQ(table[9].max_value, 64.0f);
}

TEST(mod_lfo_shapes_deterministic) {
    doc::ModSource lfo;
    lfo.type = doc::ModSourceType::Lfo;
    lfo.rate_hz = 1.0f;

    lfo.shape = doc::LfoShape::Sine;
    CHECK(near(mod::eval_source(lfo, 0.0, 0, nullptr), 0.5f));
    CHECK(near(mod::eval_source(lfo, 0.25, 0, nullptr), 1.0f));
    CHECK(near(mod::eval_source(lfo, 0.75, 0, nullptr), 0.0f));

    lfo.shape = doc::LfoShape::Triangle;
    CHECK(near(mod::eval_source(lfo, 0.5, 0, nullptr), 1.0f));
    CHECK(near(mod::eval_source(lfo, 0.25, 0, nullptr), 0.5f));

    lfo.shape = doc::LfoShape::Square;
    CHECK(near(mod::eval_source(lfo, 0.1, 0, nullptr), 1.0f));
    CHECK(near(mod::eval_source(lfo, 0.6, 0, nullptr), 0.0f));

    // S&H: constant within a cycle, deterministic across calls.
    lfo.shape = doc::LfoShape::SampleHold;
    lfo.seed = 42;
    const float a = mod::eval_source(lfo, 0.1, 0, nullptr);
    const float b = mod::eval_source(lfo, 0.9, 0, nullptr);
    const float c = mod::eval_source(lfo, 1.1, 0, nullptr);
    CHECK_EQ(a, b);
    CHECK(a != c);
    CHECK_EQ(a, mod::eval_source(lfo, 0.1, 0, nullptr));
}

TEST(mod_video_sampling_sources) {
    // Synthetic I420 frame: left half black, right half white, flat gray
    // chroma. 32x16 keeps the tap grids exact.
    constexpr int W = 32, H = 16;
    uint8_t y_plane[W * H];
    uint8_t u_plane[(W / 2) * (H / 2)];
    uint8_t v_plane[(W / 2) * (H / 2)];
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            y_plane[r * W + c] = c < W / 2 ? 0 : 255;
    for (int i = 0; i < (W / 2) * (H / 2); ++i) u_plane[i] = v_plane[i] = 128;
    mod::SourceFrameView view;
    view.y = y_plane;
    view.y_stride = W;
    view.u = u_plane;
    view.u_stride = W / 2;
    view.v = v_plane;
    view.v_stride = W / 2;
    view.width = W;
    view.height = H;

    doc::ModSource s;
    s.type = doc::ModSourceType::VideoSample;
    s.px = 0.85f;   // deep inside the white half (clear of the box blur)
    s.py = 0.5f;
    CHECK(mod::eval_source(s, 0.0, 0, nullptr, 30.0, 0.0, -1.0, &view) >
          0.95f);
    s.px = 0.15f;
    CHECK(mod::eval_source(s, 0.0, 0, nullptr, 30.0, 0.0, -1.0, &view) <
          0.05f);
    // No frame view -> inert 0 (the speed target contract).
    CHECK_EQ(mod::eval_source(s, 0.0, 0, nullptr), 0.0f);

    // Region mean over the full frame straddles both halves.
    s.type = doc::ModSourceType::VideoRegion;
    s.px = s.py = 0.5f;
    s.pw = s.ph = 1.0f;
    const float mean =
        mod::eval_source(s, 0.0, 0, nullptr, 30.0, 0.0, -1.0, &view);
    CHECK(mean > 0.35f && mean < 0.65f);
    // Deterministic: identical inputs, identical value.
    CHECK_EQ(mean,
             mod::eval_source(s, 0.0, 0, nullptr, 30.0, 0.0, -1.0, &view));

    // A route on a video source resolves through the same view.
    doc::Document d = make_doc();
    doc::ModRoute route;
    route.id = 1;
    route.source = s;
    route.target = {d.layers[0].stack[0].id, doc::kWetParam};
    route.amount = -1.0f;
    d.mod_routes.push_back(route);
    const doc::Document lit =
        mod::resolve(d, 0, 30.0, nullptr, -1.0, -1.0, &view);
    CHECK(lit.layers[0].stack[0].wet < 1.0f);
    const doc::Document dark = mod::resolve(d, 0, 30.0, nullptr);
    CHECK_EQ(dark.layers[0].stack[0].wet, 1.0f);
}

TEST(mod_lane_eval) {
    doc::KeyframeLane lane;
    lane.keys.push_back({0.0, 0.0f});
    lane.keys.push_back({10.0, 1.0f});
    // Zero handles = linear.
    CHECK(near(mod::eval_lane(lane, 5.0), 0.5f));
    CHECK(near(mod::eval_lane(lane, -3.0), 0.0f));   // clamp before
    CHECK(near(mod::eval_lane(lane, 20.0), 1.0f));   // clamp after

    // Hold key steps.
    lane.keys[0].hold = true;
    CHECK(near(mod::eval_lane(lane, 9.9), 0.0f));
    CHECK(near(mod::eval_lane(lane, 10.0), 1.0f));

    // Bezier ease (flat-out handle): midpoint pulls below linear.
    lane.keys[0].hold = false;
    lane.keys[0].out_dx = 5.0f;
    lane.keys[0].out_dy = 0.0f;
    const float eased = mod::eval_lane(lane, 5.0);
    CHECK(eased < 0.5f);
    CHECK(eased > 0.0f);
    // Endpoints exact.
    CHECK(near(mod::eval_lane(lane, 0.0), 0.0f));
    CHECK(near(mod::eval_lane(lane, 10.0), 1.0f));
}

TEST(mod_resolve_lane_and_route) {
    doc::Document d = make_doc();
    const uint64_t vignette_id = d.layers[0].stack[0].id;

    // Lane drives vignette amount from 0 to 1 over 10 frames.
    doc::KeyframeLane lane;
    lane.target = {vignette_id, 0};   // amount
    lane.keys.push_back({0.0, 0.0f});
    lane.keys.push_back({10.0, 1.0f});
    d.lanes.push_back(lane);

    doc::Document r0 = mod::resolve(d, 0, 30.0, nullptr);
    doc::Document r5 = mod::resolve(d, 5, 30.0, nullptr);
    CHECK(near(r0.layers[0].stack[0].params[0], 0.0f));
    CHECK(near(r5.layers[0].stack[0].params[0], 0.5f));
    // Source doc untouched.
    CHECK(near(d.layers[0].stack[0].params[0], 0.6f));

    // Route: square LFO at 1 Hz on wet, amount 0.5 → +0.5 in first half.
    doc::ModRoute route;
    route.id = 1;
    route.target = {vignette_id, doc::kWetParam};
    route.amount = -0.5f;
    route.source.type = doc::ModSourceType::Lfo;
    route.source.shape = doc::LfoShape::Square;
    route.source.rate_hz = 1.0f;
    d.mod_routes.push_back(route);

    // Frame 0: square=1 → wet = 1 + (-0.5)*1 = 0.5. Frame 15 (t=0.5s):
    // square=0 → wet stays 1.
    doc::Document ra = mod::resolve(d, 0, 30.0, nullptr);
    doc::Document rb = mod::resolve(d, 15, 30.0, nullptr);
    CHECK(near(ra.layers[0].stack[0].wet, 0.5f));
    CHECK(near(rb.layers[0].stack[0].wet, 1.0f));

    // Clamped at range edges.
    d.mod_routes[0].amount = -2.0f;
    doc::Document rc = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(near(rc.layers[0].stack[0].wet, 0.0f));
}

TEST(mod_resolve_analysis_sources) {
    doc::Document d = make_doc();
    doc::ModRoute route;
    route.id = 1;
    route.target = {d.layers[0].stack[1].id, 0};   // rgb shift_x, range -64..64
    route.amount = 0.25f;
    route.source.type = doc::ModSourceType::AudioLow;
    d.mod_routes.push_back(route);

    mod::AnalysisCurves curves;
    curves.low = {0.0f, 1.0f, 0.5f};
    // base 6 + 0.25*128*low
    doc::Document r0 = mod::resolve(d, 0, 30.0, &curves);
    doc::Document r1 = mod::resolve(d, 1, 30.0, &curves);
    doc::Document r9 = mod::resolve(d, 9, 30.0, &curves);   // clamps to last
    CHECK(near(r0.layers[0].stack[1].params[0], 6.0f));
    CHECK(near(r1.layers[0].stack[1].params[0], 38.0f));
    CHECK(near(r9.layers[0].stack[1].params[0], 22.0f));
}

TEST(mod_route_commands_undo) {
    doc::Document d = make_doc();
    doc::UndoStack undo;

    doc::ModRoute route;
    route.id = d.next_route_id++;
    route.target = {d.layers[0].stack[0].id, doc::kWetParam};
    route.amount = 0.3f;
    undo.execute(d, doc::add_route_command(route));
    CHECK_EQ(d.mod_routes.size(), size_t{1});

    undo.execute(d, doc::set_route_amount_command(route.id, 0.5f), true);
    undo.execute(d, doc::set_route_amount_command(route.id, 0.7f), true);
    CHECK(near(d.mod_routes[0].amount, 0.7f));
    undo.undo(d);   // one step back through the coalesced drag
    CHECK(near(d.mod_routes[0].amount, 0.3f));
    undo.redo(d);
    CHECK(near(d.mod_routes[0].amount, 0.7f));

    undo.execute(d, doc::remove_route_command(route.id));
    CHECK(d.mod_routes.empty());
    undo.undo(d);
    CHECK_EQ(d.mod_routes.size(), size_t{1});
    CHECK(near(d.mod_routes[0].amount, 0.7f));
}

TEST(mod_lane_command_and_snapshots) {
    doc::Document d = make_doc();
    doc::UndoStack undo;
    const doc::ParamKey key{d.layers[0].stack[0].id, 0};

    undo.execute(d, doc::set_lane_command(key, {{0.0, 0.1f}, {5.0, 0.9f}}));
    CHECK_EQ(d.lanes.size(), size_t{1});
    CHECK_EQ(d.lanes[0].keys.size(), size_t{2});

    // Empty keys removes the lane; undo restores it.
    undo.execute(d, doc::set_lane_command(key, {}));
    CHECK(d.lanes.empty());
    undo.undo(d);
    CHECK_EQ(d.lanes.size(), size_t{1});

    // Snapshots: store A, mutate, apply A restores params.
    undo.execute(d, doc::store_snapshot_command(0));
    CHECK(d.snapshots[0].valid);
    undo.execute(d, doc::set_param_command(0, 0, 0, 0.11f));
    CHECK(near(d.layers[0].stack[0].params[0], 0.11f));
    undo.execute(d, doc::apply_snapshot_command(0));
    CHECK(near(d.layers[0].stack[0].params[0], 0.6f));
    undo.undo(d);   // un-apply
    CHECK(near(d.layers[0].stack[0].params[0], 0.11f));
}

TEST(mod_fft_sine_bin) {
    // 1 kHz sine at 48 kHz, 1024-point FFT → energy concentrated at bin
    // round(1000/46.875) = 21.
    constexpr size_t kN = 1024;
    std::vector<float> signal(kN);
    for (size_t i = 0; i < kN; ++i)
        signal[i] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979 * 1000.0 * i / 48000.0));
    auto mags = mod::magnitude_spectrum(signal.data(), kN, kN);
    size_t peak = 0;
    for (size_t i = 1; i < mags.size(); ++i)
        if (mags[i] > mags[peak]) peak = i;
    CHECK(peak >= 20 && peak <= 22);
    // Peak dominates the far spectrum.
    CHECK(mags[peak] > 20.0f * mags[400]);
}

TEST(mod_analysis_roundtrip) {
    mod::AnalysisData data;
    data.fps = 30.0;
    data.frame_count = 4;
    data.bpm = 120.0f;
    data.low = {0.1f, 0.2f, 0.3f, 0.4f};
    data.mid = {0.5f, 0.5f, 0.5f, 0.5f};
    data.high = {1.0f, 0.0f, 1.0f, 0.0f};
    data.onset = {0.0f, 1.0f, 0.0f, 0.0f};
    data.motion = {0.0f, 0.25f, 0.5f, 0.75f};
    data.brightness = {0.9f, 0.9f, 0.9f, 0.9f};
    data.cut = {0.0f, 0.0f, 1.0f, 0.0f};

    const auto path =
        std::filesystem::temp_directory_path() / "looks_test.analysis";
    CHECK(mod::write_analysis(path, data));
    mod::AnalysisData loaded;
    CHECK(mod::load_analysis(path, &loaded));
    CHECK_EQ(loaded.frame_count, 4u);
    CHECK(near(static_cast<float>(loaded.fps), 30.0f));
    CHECK(near(loaded.bpm, 120.0f));
    CHECK(loaded.low == data.low);
    CHECK(loaded.onset == data.onset);
    CHECK(loaded.motion == data.motion);
    CHECK(loaded.cut == data.cut);
    std::filesystem::remove(path);   // test-owned temp fixture
}

TEST(mod_analyze_audio_bands) {
    // 2 s of 100 Hz sine → low band lights up, high stays near zero.
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kFrames = 60;   // 2 s at 30 fps
    std::vector<int16_t> pcm(kRate * 2);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(
            std::sin(2.0 * 3.14159265358979 * 100.0 * i / kRate) * 20000.0);

    mod::AnalysisData data;
    mod::analyze_audio(pcm.data(), pcm.size(), 1, kRate, 30.0, kFrames, &data);
    CHECK_EQ(data.low.size(), size_t{kFrames});
    float low_sum = 0.0f, high_sum = 0.0f;
    for (uint32_t f = 5; f < kFrames - 5; ++f) {
        low_sum += data.low[f];
        high_sum += data.high[f];
    }
    CHECK(low_sum > 20.0f);          // sustained low-band energy
    CHECK(high_sum < low_sum * 0.1f);
}

TEST(mod_time_remap_identity_and_modes) {
    doc::Document d;
    mod::TimeRemap remap;
    // 1x forward = identity (and clamps to the last frame).
    CHECK(!mod::time_remap_active(d));
    CHECK_EQ(remap.source_frame(d, 0, 30.0, nullptr, 10), 0u);
    CHECK_EQ(remap.source_frame(d, 7, 30.0, nullptr, 10), 7u);
    CHECK_EQ(remap.source_frame(d, 42, 30.0, nullptr, 10), 9u);

    // 2x forward wraps around the clip.
    d.speed = 2.0f;
    CHECK(mod::time_remap_active(d));
    mod::TimeRemap fwd;
    CHECK_EQ(fwd.source_frame(d, 0, 30.0, nullptr, 10), 0u);
    CHECK_EQ(fwd.source_frame(d, 3, 30.0, nullptr, 10), 6u);
    CHECK_EQ(fwd.source_frame(d, 6, 30.0, nullptr, 10), 2u);   // 12 % 10

    // Reverse at 1x runs from the end.
    d.speed = 1.0f;
    d.time_mode = 1;
    mod::TimeRemap rev;
    CHECK_EQ(rev.source_frame(d, 0, 30.0, nullptr, 10), 9u);
    CHECK_EQ(rev.source_frame(d, 4, 30.0, nullptr, 10), 5u);
    CHECK_EQ(rev.source_frame(d, 9, 30.0, nullptr, 10), 0u);

    // Ping-pong folds: 0..9 then back down.
    d.time_mode = 2;
    mod::TimeRemap pong;
    CHECK_EQ(pong.source_frame(d, 9, 30.0, nullptr, 10), 9u);
    CHECK_EQ(pong.source_frame(d, 12, 30.0, nullptr, 10), 6u);
    CHECK_EQ(pong.source_frame(d, 18, 30.0, nullptr, 10), 0u);
    CHECK_EQ(pong.source_frame(d, 20, 30.0, nullptr, 10), 2u);
}

TEST(mod_time_remap_seek_matches_sequential) {
    // A ramped speed lane: the prefix sum after a cold seek must equal the
    // incrementally-accumulated one (determinism, spec S11).
    doc::Document d;
    doc::KeyframeLane lane;
    lane.target = {0, 1};   // global.speed
    lane.keys = {{0.0, 0.5f}, {30.0, 3.0f}};
    d.lanes.push_back(lane);
    CHECK(mod::time_remap_active(d));

    mod::TimeRemap sequential, seek;
    std::vector<uint32_t> expect;
    for (uint32_t f = 0; f <= 40; ++f)
        expect.push_back(sequential.source_frame(d, f, 30.0, nullptr, 90));
    // Cold instance jumping straight to each frame — identical mapping.
    for (uint32_t f = 0; f <= 40; f += 7) {
        mod::TimeRemap cold;
        CHECK_EQ(cold.source_frame(d, f, 30.0, nullptr, 90), expect[f]);
    }
    // Backward seek on a warm instance recomputes correctly.
    CHECK_EQ(seek.source_frame(d, 40, 30.0, nullptr, 90), expect[40]);
    CHECK_EQ(seek.source_frame(d, 12, 30.0, nullptr, 90), expect[12]);
    CHECK_EQ(seek.source_frame(d, 13, 30.0, nullptr, 90), expect[13]);
}

TEST(mod_speed_at_lane_and_route) {
    doc::Document d;
    CHECK(near(mod::speed_at(d, 0, 30.0, nullptr), 1.0f));
    d.speed = 2.5f;
    CHECK(near(mod::speed_at(d, 5, 30.0, nullptr), 2.5f));
    // A lane on {0, 1} overrides the base...
    doc::KeyframeLane lane;
    lane.target = {0, 1};
    lane.keys = {{0.0, 1.0f}, {10.0, 2.0f}};
    d.lanes.push_back(lane);
    CHECK(near(mod::speed_at(d, 0, 30.0, nullptr), 1.0f));
    CHECK(near(mod::speed_at(d, 5, 30.0, nullptr), 1.5f));
    // ...and clamping holds at the range edge.
    d.lanes[0].keys = {{0.0, 100.0f}};
    CHECK(near(mod::speed_at(d, 0, 30.0, nullptr), doc::kMaxSpeed));
}

TEST(mod_envelope_source) {
    mod::AnalysisCurves curves;
    curves.onset.assign(40, 0.0f);
    curves.onset[10] = 1.0f;
    curves.cut.assign(40, 0.0f);
    curves.cut[4] = 1.0f;

    doc::ModSource env;
    env.type = doc::ModSourceType::Envelope;
    env.attack = 0.02f;
    env.decay = 0.3f;

    auto at = [&](uint32_t f) {
        return mod::eval_source(env, f / 30.0, f, &curves, 30.0);
    };
    CHECK_EQ(at(5), 0.0f);            // before the trigger
    CHECK(at(10) > 0.5f);             // burst on the trigger frame
    CHECK(at(11) > at(20));           // decaying
    CHECK(at(20) > at(30));
    CHECK(at(30) > 0.0f);
    // Deterministic: a cold re-evaluation matches (pure function of frame).
    CHECK_EQ(at(20), at(20));

    env.trigger = 1;                  // scene-cut trigger instead
    CHECK_EQ(mod::eval_source(env, 0.0, 0, &curves, 30.0), 0.0f);
    CHECK(mod::eval_source(env, 4 / 30.0, 4, &curves, 30.0) > 0.5f);
}

TEST(mod_lfo_beat_synced) {
    mod::AnalysisCurves curves;
    curves.bpm = 120.0f;              // 2 beats/s -> 0.5 s per cycle at x1

    doc::ModSource lfo;
    lfo.type = doc::ModSourceType::LfoBeat;
    lfo.shape = doc::LfoShape::Square;
    lfo.rate_hz = 1.0f;               // beats per cycle

    CHECK_EQ(mod::eval_source(lfo, 0.1, 3, &curves, 30.0), 1.0f);
    CHECK_EQ(mod::eval_source(lfo, 0.3, 9, &curves, 30.0), 0.0f);
    CHECK_EQ(mod::eval_source(lfo, 0.6, 18, &curves, 30.0), 1.0f);
    // Two beats per cycle: period doubles.
    lfo.rate_hz = 2.0f;
    CHECK_EQ(mod::eval_source(lfo, 0.3, 9, &curves, 30.0), 1.0f);
    // No analysis: falls back to 120 BPM instead of going silent.
    lfo.rate_hz = 1.0f;
    CHECK_EQ(mod::eval_source(lfo, 0.1, 3, nullptr, 30.0), 1.0f);
}

TEST(mod_video_cut_source) {
    mod::AnalysisCurves curves;
    curves.cut = {0.0f, 1.0f, 0.0f};
    doc::ModSource src;
    src.type = doc::ModSourceType::VideoCut;
    CHECK_EQ(mod::eval_source(src, 0.0, 0, &curves, 30.0), 0.0f);
    CHECK_EQ(mod::eval_source(src, 0.033, 1, &curves, 30.0), 1.0f);
    CHECK_EQ(mod::eval_source(src, 0.0, 0, nullptr, 30.0), 0.0f);
}

TEST(mod_mask_param_targets) {
    doc::Document d;
    doc::Mask mask;
    mask.id = d.next_mask_id++;
    mask.name = "m";
    mask.feather = 0.1f;
    d.masks.push_back(mask);

    // The table exposes mask params under the namespaced key (spec S8).
    auto table = mod::build_param_table(d);
    bool found = false;
    for (const auto& e : table)
        if (e.path == "mask.m.feather") {
            found = true;
            CHECK_EQ(e.key.effect_id, mask.id | doc::kMaskParamBit);
            CHECK(near(e.base, 0.1f));
        }
    CHECK(found);

    // Square LFO route on feather: +amount*range in the first half-cycle.
    doc::ModRoute route;
    route.id = 1;
    route.target = {mask.id | doc::kMaskParamBit, 0};
    route.amount = 0.5f;
    route.source.type = doc::ModSourceType::Lfo;
    route.source.shape = doc::LfoShape::Square;
    route.source.rate_hz = 1.0f;
    d.mod_routes.push_back(route);
    doc::Document r0 = mod::resolve(d, 0, 30.0, nullptr);
    doc::Document r15 = mod::resolve(d, 15, 30.0, nullptr);
    CHECK(near(r0.masks[0].feather, 0.1f + 0.5f * 0.5f));   // feather max 0.5
    CHECK(near(r15.masks[0].feather, 0.1f));

    // A lane sets the base.
    d.mod_routes.clear();
    doc::KeyframeLane lane;
    lane.target = {mask.id | doc::kMaskParamBit, 0};
    lane.keys = {{0.0, 0.0f}, {10.0, 0.4f}};
    d.lanes.push_back(lane);
    doc::Document r5 = mod::resolve(d, 5, 30.0, nullptr);
    CHECK(near(r5.masks[0].feather, 0.2f));
}

TEST(mod_mask_point_targets) {
    doc::Document d;
    doc::Mask mask;
    mask.id = d.next_mask_id++;
    mask.name = "blob";
    mask.points = {0.2f, 0.2f, 0.8f, 0.2f, 0.8f, 0.8f, 0.2f, 0.8f};
    d.masks.push_back(mask);

    // Every path point coordinate is addressable (spec S8 "keyframable
    // points"): p<i>.x / p<i>.y past kMaskPointParamBase.
    auto table = mod::build_param_table(d);
    bool found = false;
    for (const auto& e : table)
        if (e.path == "mask.blob.p1.x") {
            found = true;
            CHECK_EQ(e.key.effect_id, mask.id | doc::kMaskParamBit);
            CHECK_EQ(e.key.param_index, doc::kMaskPointParamBase + 2);
            CHECK(near(e.base, 0.8f));
        }
    CHECK(found);

    // A lane on p1.x animates the resolved shape; unkeyed coords stay put.
    doc::KeyframeLane lane;
    lane.target = {mask.id | doc::kMaskParamBit,
                   doc::kMaskPointParamBase + 2};
    lane.keys = {{0.0, 0.2f}, {10.0, 0.6f}};
    d.lanes.push_back(lane);
    CHECK(near(mod::resolve(d, 0, 30.0, nullptr).masks[0].points[2], 0.2f));
    CHECK(near(mod::resolve(d, 5, 30.0, nullptr).masks[0].points[2], 0.4f));
    CHECK(near(mod::resolve(d, 10, 30.0, nullptr).masks[0].points[2], 0.6f));
    CHECK(near(mod::resolve(d, 5, 30.0, nullptr).masks[0].points[3], 0.2f));
}

TEST(mod_set_lanes_command_atomic) {
    doc::Document d;
    doc::UndoStack undo;
    const doc::ParamKey kx{7ull | doc::kMaskParamBit,
                           doc::kMaskPointParamBase};
    const doc::ParamKey ky{7ull | doc::kMaskParamBit,
                           doc::kMaskPointParamBase + 1};

    undo.execute(d, doc::set_lanes_command({{kx, {{0.0, 0.1f}}},
                                            {ky, {{0.0, 0.9f}}}}), true);
    CHECK_EQ(d.lanes.size(), size_t{2});
    undo.execute(d, doc::set_lanes_command({{kx, {{0.0, 0.2f}}},
                                            {ky, {{0.0, 0.8f}}}}), true);
    CHECK_EQ(d.lanes.size(), size_t{2});
    CHECK(near(d.lanes[0].keys[0].value, 0.2f));
    CHECK(near(d.lanes[1].keys[0].value, 0.8f));

    // The coalesced drag is one undo step; both lanes revert atomically.
    undo.undo(d);
    CHECK(d.lanes.empty());
    undo.redo(d);
    CHECK_EQ(d.lanes.size(), size_t{2});
    CHECK(near(d.lanes[0].keys[0].value, 0.2f));
}

TEST(mod_remove_mask_point_shifts_lanes) {
    doc::Document d;
    doc::Mask mask;
    mask.id = d.next_mask_id++;
    mask.points = {0.2f, 0.2f, 0.8f, 0.2f, 0.5f, 0.8f};
    d.masks.push_back(mask);
    const uint64_t key_id = mask.id | doc::kMaskParamBit;

    // Lanes on p0.x, p1.y, p2.x — plus a non-point lane that must not move.
    d.lanes.push_back({{key_id, 0}, {{0.0, 0.1f}}});
    d.lanes.push_back({{key_id, doc::kMaskPointParamBase}, {{0.0, 0.25f}}});
    d.lanes.push_back(
        {{key_id, doc::kMaskPointParamBase + 3}, {{0.0, 0.5f}}});
    d.lanes.push_back(
        {{key_id, doc::kMaskPointParamBase + 4}, {{0.0, 0.75f}}});

    doc::UndoStack undo;
    undo.execute(d, doc::remove_mask_point_command(mask.id, 1));
    CHECK_EQ(d.masks[0].points.size(), size_t{4});
    CHECK(near(d.masks[0].points[2], 0.5f));   // p2 slid into slot 1
    // p1.y lane died; p2.x retargeted to p1.x; the rest untouched.
    CHECK_EQ(d.lanes.size(), size_t{3});
    CHECK_EQ(d.lanes[0].target.param_index, 0);
    CHECK_EQ(d.lanes[1].target.param_index, doc::kMaskPointParamBase);
    CHECK_EQ(d.lanes[2].target.param_index, doc::kMaskPointParamBase + 2);

    undo.undo(d);
    CHECK_EQ(d.masks[0].points.size(), size_t{6});
    CHECK_EQ(d.lanes.size(), size_t{4});
    CHECK_EQ(d.lanes[2].target.param_index, doc::kMaskPointParamBase + 3);
    CHECK(near(d.lanes[2].keys[0].value, 0.5f));
    CHECK_EQ(d.lanes[3].target.param_index, doc::kMaskPointParamBase + 4);
}
