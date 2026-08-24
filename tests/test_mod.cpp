#include <cmath>

#include "doc/effects.h"
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
    d.looks[0].layers[0].stack.push_back(doc::make_effect(d, doc::EffectType::Vignette));
    d.looks[0].layers[0].stack.push_back(doc::make_effect(d, doc::EffectType::RgbSplit));
    return d;
}

bool near(float a, float b, float eps = 1.0e-4f) {
    return std::fabs(a - b) <= eps;
}

// A generator node wired onto one param: the wire REPLACES the param's
// base with the node's output mapped onto its range.
uint64_t add_valued_route(doc::Document& d, doc::ModSource source,
                          doc::ParamKey target) {
    doc::ValueNode node;
    node.id = d.next_route_id++;
    node.source = source;
    d.looks[0].value_nodes.push_back(node);
    doc::ModRoute route;
    route.id = d.next_route_id++;
    route.node = node.id;
    route.target = target;
    d.looks[0].mod_routes.push_back(route);
    return node.id;
}

}  // namespace

TEST(mod_param_table_paths) {
    doc::Document d = make_doc();
    auto table = mod::build_param_table(d, d.looks[0]);
    // global.morph + vignette (wet, opacity, amount, radius,
    // softness = 5) + rgb split (4) + layer params (opacity, colors,
    // gen + phase, transform — the modulatable slots; slip and the
    // waveform selector are field ids with no slot). Project speed is
    // a scalar, not a mod target.
    CHECK_EQ(table.size(), size_t{10 + doc::kLayerParamCount - 2});
    CHECK_EQ(table[0].path, "global.morph");
    CHECK_EQ(table[0].key.effect_id, uint64_t{0});
    CHECK_EQ(table[1].path, "layer0.fx0.wet");
    CHECK_EQ(table[3].path, "layer0.fx0.amount");
    CHECK_EQ(table[6].path, "layer0.fx1.wet");
    CHECK_EQ(table[8].path, "layer0.fx1.shift_x");
    CHECK_EQ(table[8].key.effect_id, d.looks[0].layers[0].stack[1].id);
    CHECK_EQ(table[8].min_value, -64.0f);
    CHECK_EQ(table[8].max_value, 64.0f);
    // Layer entries carry kLayerParamBit + the layer id.
    CHECK_EQ(table[10].path, "layer0.opacity");
    CHECK_EQ(table[10].key.effect_id,
             d.looks[0].layers[0].id | doc::kLayerParamBit);
    CHECK_EQ(table[10].key.param_index, 0);
    CHECK_EQ(table[24].path, "layer0.xf_rotate");
    CHECK_EQ(table[24].min_value, -180.0f);
}

TEST(mod_resolve_snaps_discrete_params) {
    // Integer-semantics params (counts, selectors) snap to whole numbers
    // after lanes/routes/morph — fractional level counts alias the
    // kernel math (a dither `levels` of 2.2 cuts a band into the frame).
    doc::Document d;
    d.looks[0].layers[0].stack.push_back(
        doc::make_effect(d, doc::EffectType::Dither));
    doc::KeyframeLane lane;
    lane.target = {d.looks[0].layers[0].stack[0].id, 0};   // levels, integer count
    lane.keys.push_back({0.0, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    lane.keys.push_back({10.0, 7.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    d.looks[0].lanes.push_back(lane);
    // A continuous param lane stays fractional: dither amount (index 2).
    doc::KeyframeLane amt;
    amt.target = {d.looks[0].layers[0].stack[0].id, 2};
    amt.keys.push_back({0.0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    amt.keys.push_back({10.0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    d.looks[0].lanes.push_back(amt);

    const doc::Document r = mod::resolve(d, 3, 30.0, nullptr);
    const float levels = r.looks[0].layers[0].stack[0].params[0];
    CHECK_EQ(levels, std::round(levels));
    CHECK(levels >= 2.0f && levels <= 7.0f);
    const float amount = r.looks[0].layers[0].stack[0].params[2];
    CHECK(amount > 0.05f && amount < 0.95f);
    CHECK(amount != std::round(amount));
}

TEST(mod_resolve_layer_params) {
    // Layer params are mod targets (kLayerParamBit): a lane on the
    // gradient angle drives the resolved layer field.
    doc::Document d;
    doc::KeyframeLane lane;
    lane.target = {d.looks[0].layers[0].id | doc::kLayerParamBit, 8};   // gen_angle
    lane.keys.push_back({0.0, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    lane.keys.push_back({10.0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    d.looks[0].lanes.push_back(lane);
    const doc::Document r0 = mod::resolve(d, 0, 30.0, nullptr);
    const doc::Document r10 = mod::resolve(d, 10, 30.0, nullptr);
    CHECK(near(r0.looks[0].layers[0].gen_angle, -1.0f));
    CHECK(near(r10.looks[0].layers[0].gen_angle, 1.0f));
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

    // A route on a video source resolves through the same view: the
    // wire replaces wet with the region mean; without a view the source
    // reads 0 and drives wet to the range floor.
    doc::Document d = make_doc();
    add_valued_route(d, s,
                     {d.looks[0].layers[0].stack[0].id, doc::kWetParam});
    const doc::Document lit =
        mod::resolve(d, 0, 30.0, nullptr, -1.0, -1.0, &view);
    CHECK(near(lit.looks[0].layers[0].stack[0].wet, mean));
    const doc::Document dark = mod::resolve(d, 0, 30.0, nullptr);
    CHECK_EQ(dark.looks[0].layers[0].stack[0].wet, 0.0f);
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
    const uint64_t vignette_id = d.looks[0].layers[0].stack[0].id;

    // Lane drives vignette amount from 0 to 1 over 10 frames.
    doc::KeyframeLane lane;
    lane.target = {vignette_id, 0};   // amount
    lane.keys.push_back({0.0, 0.0f});
    lane.keys.push_back({10.0, 1.0f});
    d.looks[0].lanes.push_back(lane);

    doc::Document r0 = mod::resolve(d, 0, 30.0, nullptr);
    doc::Document r5 = mod::resolve(d, 5, 30.0, nullptr);
    CHECK(near(r0.looks[0].layers[0].stack[0].params[0], 0.0f));
    CHECK(near(r5.looks[0].layers[0].stack[0].params[0], 0.5f));
    // Source doc untouched.
    CHECK(near(d.looks[0].layers[0].stack[0].params[0], 0.6f));

    // Route: square LFO at 1 Hz REPLACES wet. Frame 0: square=1 →
    // wet 1. Frame 15 (t=0.5s): square=0 → wet 0 (the stored base 1 is
    // ignored while wired).
    doc::ModSource lfo;
    lfo.type = doc::ModSourceType::Lfo;
    lfo.shape = doc::LfoShape::Square;
    lfo.rate_hz = 1.0f;
    add_valued_route(d, lfo, {vignette_id, doc::kWetParam});

    doc::Document ra = mod::resolve(d, 0, 30.0, nullptr);
    doc::Document rb = mod::resolve(d, 15, 30.0, nullptr);
    CHECK(near(ra.looks[0].layers[0].stack[0].wet, 1.0f));
    CHECK(near(rb.looks[0].layers[0].stack[0].wet, 0.0f));

    // A chain past the range clamps at the param's edge: constant 2
    // through a math node still lands wet on 1.
    doc::ValueNode big;
    big.id = d.next_route_id++;
    big.source.type = doc::ModSourceType::Math;
    big.op = doc::ValueOp::Add;
    big.const_a = 2.0f;
    big.const_b = 0.0f;
    d.looks[0].value_nodes.push_back(big);
    d.looks[0].mod_routes[0].node = big.id;
    doc::Document rc = mod::resolve(d, 15, 30.0, nullptr);
    CHECK(near(rc.looks[0].layers[0].stack[0].wet, 1.0f));
}

TEST(mod_wired_analysis_node_reads_its_connection) {
    // An analysis node REQUIRES its input: the wired connection's
    // curves sample at the look clock plus the chain's slip; a wire
    // with no entry and an unwired node both read 0 - never the
    // global set.
    doc::Document d;
    doc::Look& look = d.looks[0];
    look.layers[0].asset = d.next_effect_id++;
    doc::ValueNode n;
    n.id = d.next_effect_id++;
    n.source.type = doc::ModSourceType::AudioLow;
    n.audio_src = look.layers[0].id;
    look.value_nodes.push_back(n);

    auto curves = std::make_shared<mod::AnalysisCurves>();
    curves->low = {0.1f, 0.2f, 0.3f, 0.4f};
    mod::NodeAudioMap map;
    map[n.id] = {curves, 1};   // slip 1: local frame 2 reads curve[3]

    mod::ValueEnv env;
    env.look = &look;
    env.frame = 2;
    env.t = 2.0 / 30.0;
    env.node_audio = &map;
    CHECK(near(mod::eval_value_node(env, n.id), 0.4f));

    mod::AnalysisCurves global;
    global.low = {0.9f, 0.9f, 0.9f, 0.9f};
    env.analysis = &global;
    mod::NodeAudioMap empty;
    env.node_audio = &empty;
    CHECK_EQ(mod::eval_value_node(env, n.id), 0.0f);

    look.value_nodes[0].audio_src = 0;
    CHECK_EQ(mod::eval_value_node(env, n.id), 0.0f);
}

TEST(mod_resolve_analysis_sources) {
    // Analysis nodes require their input: the wired connection's
    // curves drive the param through resolve(); unwired reads 0 (the
    // range floor) even with global curves present.
    doc::Document d = make_doc();
    doc::ModSource low;
    low.type = doc::ModSourceType::AudioLow;
    // rgb shift_x, range -64..64: the wire maps low onto the full span.
    add_valued_route(d, low, {d.looks[0].layers[0].stack[1].id, 0});
    doc::ValueNode& vn = d.looks[0].value_nodes.back();
    vn.audio_src = d.looks[0].layers[0].id;

    auto curves = std::make_shared<mod::AnalysisCurves>();
    curves->low = {0.0f, 1.0f, 0.5f};
    mod::NodeAudioMap map;
    map[vn.id] = {curves, 0};
    // shift = -64 + 128*low
    doc::Document r0 =
        mod::resolve(d, 0, 30.0, nullptr, -1.0, -1.0, nullptr, &map);
    doc::Document r1 =
        mod::resolve(d, 1, 30.0, nullptr, -1.0, -1.0, nullptr, &map);
    doc::Document r9 =
        mod::resolve(d, 9, 30.0, nullptr, -1.0, -1.0, nullptr, &map);
    CHECK(near(r0.looks[0].layers[0].stack[1].params[0], -64.0f));
    CHECK(near(r1.looks[0].layers[0].stack[1].params[0], 64.0f));
    CHECK(near(r9.looks[0].layers[0].stack[1].params[0], 0.0f));

    d.looks[0].value_nodes.back().audio_src = 0;
    mod::AnalysisCurves global;
    global.low = {1.0f, 1.0f, 1.0f};
    doc::Document ru = mod::resolve(d, 1, 30.0, &global);
    CHECK(near(ru.looks[0].layers[0].stack[1].params[0], -64.0f));
}

TEST(mod_route_commands_undo) {
    doc::Document d = make_doc();
    doc::UndoStack undo;

    doc::ValueNode node;
    node.id = d.next_route_id++;
    undo.execute(d, doc::add_value_node_command(d.looks[0].id, node));
    CHECK_EQ(d.looks[0].value_nodes.size(), size_t{1});

    doc::ModRoute route;
    route.id = d.next_route_id++;
    route.node = node.id;
    route.target = {d.looks[0].layers[0].stack[0].id, doc::kWetParam};
    undo.execute(d, doc::add_route_command(d.looks[0].id,route));
    CHECK_EQ(d.looks[0].mod_routes.size(), size_t{1});

    // One wire per param: wiring another node onto the same target
    // REPLACES the wire; undo brings the first one back.
    doc::ValueNode node2;
    node2.id = d.next_route_id++;
    undo.execute(d, doc::add_value_node_command(d.looks[0].id, node2));
    doc::ModRoute rival;
    rival.id = d.next_route_id++;
    rival.node = node2.id;
    rival.target = route.target;
    undo.execute(d, doc::add_route_command(d.looks[0].id, rival));
    CHECK_EQ(d.looks[0].mod_routes.size(), size_t{1});
    CHECK_EQ(d.looks[0].mod_routes[0].node, node2.id);
    undo.undo(d);
    CHECK_EQ(d.looks[0].mod_routes.size(), size_t{1});
    CHECK_EQ(d.looks[0].mod_routes[0].node, node.id);
    undo.redo(d);
    CHECK_EQ(d.looks[0].mod_routes[0].node, node2.id);

    // Node edits: whole-struct replace, coalescing per id.
    doc::ValueNode edit = d.looks[0].value_nodes[0];
    edit.source.rate_hz = 3.0f;
    undo.execute(d, doc::set_value_node_command(d.looks[0].id, edit), true);
    CHECK(near(d.looks[0].value_nodes[0].source.rate_hz, 3.0f));
    undo.undo(d);
    CHECK(near(d.looks[0].value_nodes[0].source.rate_hz, 1.0f));
    undo.redo(d);

    undo.execute(d, doc::remove_route_command(d.looks[0].id,rival.id));
    CHECK(d.looks[0].mod_routes.empty());
    undo.undo(d);
    CHECK_EQ(d.looks[0].mod_routes.size(), size_t{1});
    CHECK_EQ(d.looks[0].mod_routes[0].node, node2.id);
}

TEST(mod_value_math_and_normalise) {
    doc::Document d;
    doc::Look& look = d.looks[0];
    // Constants through every math op: a = 0.6, b = 0.25.
    doc::ValueNode m;
    m.id = 1;
    m.source.type = doc::ModSourceType::Math;
    m.const_a = 0.6f;
    m.const_b = 0.25f;
    look.value_nodes.push_back(m);
    mod::ValueEnv env;
    env.look = &look;
    auto with_op = [&](doc::ValueOp op) {
        look.value_nodes[0].op = op;
        return mod::eval_value_node(env, 1);
    };
    CHECK(near(with_op(doc::ValueOp::Add), 0.85f));
    CHECK(near(with_op(doc::ValueOp::Subtract), 0.35f));
    CHECK(near(with_op(doc::ValueOp::Multiply), 0.15f));
    CHECK(near(with_op(doc::ValueOp::Divide), 2.4f));
    CHECK(near(with_op(doc::ValueOp::Min), 0.25f));
    CHECK(near(with_op(doc::ValueOp::Max), 0.6f));
    CHECK(near(with_op(doc::ValueOp::Floor), 0.0f));
    look.value_nodes[0].const_a = -0.6f;
    CHECK(near(with_op(doc::ValueOp::Absolute), 0.6f));
    // Divide by zero reads 0, not inf.
    look.value_nodes[0].const_b = 0.0f;
    CHECK_EQ(with_op(doc::ValueOp::Divide), 0.0f);

    // Normalise maps its window onto [0,1], clamped.
    doc::ValueNode norm;
    norm.id = 2;
    norm.source.type = doc::ModSourceType::Normalise;
    norm.in_min = 0.2f;
    norm.in_max = 0.7f;
    norm.const_a = 0.45f;
    look.value_nodes.push_back(norm);
    CHECK(near(mod::eval_value_node(env, 2), 0.5f));
    look.value_nodes[1].const_a = 0.9f;
    CHECK(near(mod::eval_value_node(env, 2), 1.0f));
    look.value_nodes[1].const_a = -1.0f;
    CHECK(near(mod::eval_value_node(env, 2), 0.0f));
    // Degenerate window reads 0.
    look.value_nodes[1].in_max = look.value_nodes[1].in_min;
    CHECK_EQ(mod::eval_value_node(env, 2), 0.0f);

    // The multiplier scales the window: bounds stay -1..1, magnitude
    // rides const_b. [-0.02, 1] x50 = an effective [-1, 50] window.
    look.value_nodes[1].in_min = -0.02f;
    look.value_nodes[1].in_max = 1.0f;
    look.value_nodes[1].const_b = 50.0f;
    look.value_nodes[1].const_a = 24.5f;   // window midpoint
    CHECK(near(mod::eval_value_node(env, 2), 0.5f));
    look.value_nodes[1].const_a = 50.0f;
    CHECK(near(mod::eval_value_node(env, 2), 1.0f));
    look.value_nodes[1].const_a = -1.0f;
    CHECK(near(mod::eval_value_node(env, 2), 0.0f));

    // Dangling id reads 0.
    CHECK_EQ(mod::eval_value_node(env, 99), 0.0f);
}

TEST(mod_value_chain_and_fanout) {
    doc::Document d = make_doc();
    const uint64_t vignette_id = d.looks[0].layers[0].stack[0].id;
    const uint64_t rgb_id = d.looks[0].layers[0].stack[1].id;

    // Square LFO (0/1) -> math (multiply by 0.5) -> two wires: one on
    // vignette wet, one on rgb wet. Fan-out from one shared chain.
    doc::ValueNode lfo;
    lfo.id = d.next_route_id++;
    lfo.source.type = doc::ModSourceType::Lfo;
    lfo.source.shape = doc::LfoShape::Square;
    lfo.source.rate_hz = 1.0f;
    d.looks[0].value_nodes.push_back(lfo);

    doc::ValueNode half;
    half.id = d.next_route_id++;
    half.source.type = doc::ModSourceType::Math;
    half.op = doc::ValueOp::Multiply;
    half.in_a = lfo.id;
    half.const_b = 0.5f;
    d.looks[0].value_nodes.push_back(half);

    for (const uint64_t fx : {vignette_id, rgb_id}) {
        doc::ModRoute r;
        r.id = d.next_route_id++;
        r.node = half.id;
        r.target = {fx, doc::kWetParam};
        d.looks[0].mod_routes.push_back(r);
    }

    // Frame 0: square = 1 -> chain = 0.5 -> wet REPLACED to 0.5 on
    // BOTH. Frame 15 (t = 0.5 s): square = 0 -> wet 0 on both.
    doc::Document r0 = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(near(r0.looks[0].layers[0].stack[0].wet, 0.5f));
    CHECK(near(r0.looks[0].layers[0].stack[1].wet, 0.5f));
    doc::Document r15 = mod::resolve(d, 15, 30.0, nullptr);
    CHECK(near(r15.looks[0].layers[0].stack[0].wet, 0.0f));
    CHECK(near(r15.looks[0].layers[0].stack[1].wet, 0.0f));
}

TEST(mod_value_cycle_guard) {
    doc::Document d;
    doc::Look& look = d.looks[0];
    doc::ValueNode a, b;
    a.id = 1;
    a.source.type = doc::ModSourceType::Math;
    b.id = 2;
    b.source.type = doc::ModSourceType::Math;
    look.value_nodes.push_back(a);
    look.value_nodes.push_back(b);
    look.value_nodes[0].in_a = 2;
    // Wiring b.in_a = 1 would close the loop: the guard sees it.
    CHECK(doc::value_reaches(look, 1, 2));
    CHECK(!doc::value_reaches(look, 2, 1));
    CHECK(doc::value_reaches(look, 1, 1));
    // A corrupt file's cycle still terminates at the eval depth cap.
    look.value_nodes[1].in_a = 1;
    mod::ValueEnv env;
    env.look = &look;
    (void)mod::eval_value_node(env, 1);
}

TEST(mod_remove_value_node_cascades) {
    doc::Document d = make_doc();
    doc::UndoStack undo;
    doc::Look& look = d.looks[0];
    const uint64_t vignette_id = look.layers[0].stack[0].id;

    doc::ValueNode lfo;
    lfo.id = d.next_route_id++;
    look.value_nodes.push_back(lfo);
    doc::ValueNode math;
    math.id = d.next_route_id++;
    math.source.type = doc::ModSourceType::Math;
    math.in_a = lfo.id;
    look.value_nodes.push_back(math);
    doc::ModRoute r;
    r.id = d.next_route_id++;
    r.node = lfo.id;
    r.target = {vignette_id, doc::kWetParam};
    look.mod_routes.push_back(r);

    // Removing the LFO takes its route and unwires the math input.
    undo.execute(d, doc::remove_value_node_command(look.id, lfo.id));
    CHECK_EQ(d.looks[0].value_nodes.size(), size_t{1});
    CHECK(d.looks[0].mod_routes.empty());
    CHECK_EQ(d.looks[0].value_nodes[0].in_a, uint64_t{0});
    // One undo restores the node, the wire, and the input.
    undo.undo(d);
    CHECK_EQ(d.looks[0].value_nodes.size(), size_t{2});
    CHECK_EQ(d.looks[0].mod_routes.size(), size_t{1});
    CHECK_EQ(d.looks[0].value_nodes[1].in_a, lfo.id);

    // wire_value_input swaps helper inputs undoably.
    undo.execute(d, doc::wire_value_input_command(look.id,
                     d.looks[0].value_nodes[1].id, 1, lfo.id));
    CHECK_EQ(d.looks[0].value_nodes[1].in_b, lfo.id);
    undo.undo(d);
    CHECK_EQ(d.looks[0].value_nodes[1].in_b, uint64_t{0});
}

TEST(mod_lane_command_and_snapshots) {
    doc::Document d = make_doc();
    doc::UndoStack undo;
    const doc::ParamKey key{d.looks[0].layers[0].stack[0].id, 0};

    undo.execute(d, doc::set_lane_command(d.looks[0].id,key, {{0.0, 0.1f}, {5.0, 0.9f}}));
    CHECK_EQ(d.looks[0].lanes.size(), size_t{1});
    CHECK_EQ(d.looks[0].lanes[0].keys.size(), size_t{2});

    // Empty keys removes the lane; undo restores it.
    undo.execute(d, doc::set_lane_command(d.looks[0].id,key, {}));
    CHECK(d.looks[0].lanes.empty());
    undo.undo(d);
    CHECK_EQ(d.looks[0].lanes.size(), size_t{1});

    // Snapshots: store A, mutate, apply A restores params.
    undo.execute(d, doc::store_snapshot_command(d.looks[0].id,0));
    CHECK(d.looks[0].snapshots[0].valid);
    undo.execute(d, doc::set_param_command(d.looks[0].id,0, 0, 0, 0.11f));
    CHECK(near(d.looks[0].layers[0].stack[0].params[0], 0.11f));
    undo.execute(d, doc::apply_snapshot_command(d.looks[0].id,0));
    CHECK(near(d.looks[0].layers[0].stack[0].params[0], 0.6f));
    undo.undo(d);   // un-apply
    CHECK(near(d.looks[0].layers[0].stack[0].params[0], 0.11f));
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

    // 2x forward wraps around the media.
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
    // A non-unit project speed: the prefix sum after a cold seek must
    // equal the incrementally-accumulated one (determinism). Sequences
    // carry no keyframes, so the project speed is the scalar - ramps
    // live per-block or inside looks.
    doc::Document d;
    d.speed = 1.7f;
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

TEST(mod_speed_at_is_the_project_scalar) {
    doc::Document d;
    CHECK(near(mod::speed_at(d, 0, 30.0, nullptr), 1.0f));
    CHECK(!mod::time_remap_active(d));
    d.speed = 2.5f;
    CHECK(near(mod::speed_at(d, 5, 30.0, nullptr), 2.5f));
    CHECK(mod::time_remap_active(d));
    // Clamping holds at the range edge.
    d.speed = 100.0f;
    CHECK(near(mod::speed_at(d, 0, 30.0, nullptr), doc::kMaxSpeed));
}

TEST(mod_envelope_source) {
    // Envelope's onset trigger is audio-driven: it REQUIRES the wired
    // media input and fires from the chain's onset curve at the MEDIA
    // position. The cut trigger stays video-derived (global curves).
    doc::Document d;
    doc::Look& look = d.looks[0];
    look.layers[0].asset = d.next_effect_id++;
    doc::ValueNode n;
    n.id = d.next_effect_id++;
    n.source.type = doc::ModSourceType::Envelope;
    n.source.attack = 0.02f;
    n.source.decay = 0.3f;
    n.audio_src = look.layers[0].id;
    look.value_nodes.push_back(n);

    auto curves = std::make_shared<mod::AnalysisCurves>();
    curves->onset.assign(40, 0.0f);
    curves->onset[10] = 1.0f;
    mod::NodeAudioMap map;
    map[n.id] = {curves, 0};

    mod::ValueEnv env;
    env.look = &look;
    env.node_audio = &map;
    env.fps = 30.0;
    auto at = [&](uint32_t f) {
        env.frame = f;
        env.t = f / 30.0;
        return mod::eval_value_node(env, n.id);
    };
    CHECK_EQ(at(5), 0.0f);            // before the trigger
    CHECK(at(10) > 0.5f);             // burst on the trigger frame
    CHECK(at(11) > at(20));           // decaying
    CHECK(at(20) > at(30));
    CHECK(at(30) > 0.0f);
    // Deterministic: a cold re-evaluation matches (pure function of frame).
    CHECK_EQ(at(20), at(20));

    // Unwired = never fires, global curves or not.
    mod::AnalysisCurves global;
    global.onset.assign(40, 1.0f);
    env.analysis = &global;
    look.value_nodes[0].audio_src = 0;
    CHECK_EQ(at(10), 0.0f);
    look.value_nodes[0].audio_src = look.layers[0].id;

    // The onset trigger reads 0 through eval_source: no wire there.
    doc::ModSource envs = n.source;
    CHECK_EQ(mod::eval_source(envs, 10 / 30.0, 10, &global, 30.0), 0.0f);

    // Scene-cut trigger: video-derived, fires from the global curves
    // with no wire at all.
    envs.trigger = 1;
    mod::AnalysisCurves vid;
    vid.cut.assign(40, 0.0f);
    vid.cut[4] = 1.0f;
    CHECK_EQ(mod::eval_source(envs, 0.0, 0, &vid, 30.0), 0.0f);
    CHECK(mod::eval_source(envs, 4 / 30.0, 4, &vid, 30.0) > 0.5f);
}

TEST(mod_lfo_beat_synced) {
    // Beat-synced kinds REQUIRE the wired input, take their BPM from
    // its curves, and anchor phase on the MEDIA position (local + slip
    // + offset) - cuts of one media beat-match by construction.
    doc::Document d;
    doc::Look& look = d.looks[0];
    look.layers[0].asset = d.next_effect_id++;
    doc::ValueNode n;
    n.id = d.next_effect_id++;
    n.source.type = doc::ModSourceType::LfoBeat;
    n.source.shape = doc::LfoShape::Square;
    n.source.rate_hz = 1.0f;          // beats per cycle
    n.audio_src = look.layers[0].id;
    look.value_nodes.push_back(n);

    auto curves = std::make_shared<mod::AnalysisCurves>();
    curves->bpm = 120.0f;             // 2 beats/s -> 0.5 s per cycle
    mod::NodeAudioMap map;
    map[n.id] = {curves, 0};

    mod::ValueEnv env;
    env.look = &look;
    env.node_audio = &map;
    env.fps = 30.0;
    auto at = [&](uint32_t f) {
        env.frame = f;
        env.t = f / 30.0;
        return mod::eval_value_node(env, n.id);
    };
    CHECK_EQ(at(3), 1.0f);            // 0.1 s: first half-cycle high
    CHECK_EQ(at(9), 0.0f);            // 0.3 s: low half
    CHECK_EQ(at(18), 1.0f);           // 0.6 s: high again
    // Two beats per cycle: period doubles.
    look.value_nodes[0].source.rate_hz = 2.0f;
    CHECK_EQ(at(9), 1.0f);
    look.value_nodes[0].source.rate_hz = 1.0f;

    // MEDIA anchoring: slip shifts the phase - local 3 with slip 6
    // reads the beat grid at media frame 9.
    map[n.id] = {curves, 6};
    CHECK_EQ(at(3), 0.0f);
    // An Offset shim on the chain shifts it the same way.
    map[n.id] = {curves, 0, 9};
    CHECK_EQ(at(0), 0.0f);

    // Unwired reads 0 - eval_source carries no beat clock anymore.
    doc::ModSource lfo = look.value_nodes[0].source;
    CHECK_EQ(mod::eval_source(lfo, 0.1, 3, nullptr, 30.0), 0.0f);
    mod::AnalysisCurves gcurves;
    gcurves.bpm = 120.0f;
    CHECK_EQ(mod::eval_source(lfo, 0.1, 3, &gcurves, 30.0), 0.0f);
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

TEST(mod_set_lanes_command_atomic) {
    doc::Document d;
    doc::UndoStack undo;
    const doc::ParamKey kx{7ull, 0};
    const doc::ParamKey ky{7ull, 1};

    undo.execute(d, doc::set_lanes_command(d.looks[0].id,{{kx, {{0.0, 0.1f}}},
                                            {ky, {{0.0, 0.9f}}}}), true);
    CHECK_EQ(d.looks[0].lanes.size(), size_t{2});
    undo.execute(d, doc::set_lanes_command(d.looks[0].id,{{kx, {{0.0, 0.2f}}},
                                            {ky, {{0.0, 0.8f}}}}), true);
    CHECK_EQ(d.looks[0].lanes.size(), size_t{2});
    CHECK(near(d.looks[0].lanes[0].keys[0].value, 0.2f));
    CHECK(near(d.looks[0].lanes[1].keys[0].value, 0.8f));

    // The coalesced drag is one undo step; both lanes revert atomically.
    undo.undo(d);
    CHECK(d.looks[0].lanes.empty());
    undo.redo(d);
    CHECK_EQ(d.looks[0].lanes.size(), size_t{2});
    CHECK(near(d.looks[0].lanes[0].keys[0].value, 0.2f));
}
