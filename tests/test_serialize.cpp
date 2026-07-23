#include "doc/serialize.h"

#include <cmath>
#include <filesystem>

#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/mod_commands.h"
#include "doc/preset.h"
#include "gfx/graph.h"
#include "mod/eval.h"
#include "test_framework.h"

using namespace looks;
using doc::Document;
using doc::EffectType;
using doc::make_effect;

namespace {

Document make_rich_doc() {
    Document d;
    d.name = "rich";
    d.clip_path = "C:/clips/test.mp4";
    d.master_seed = 1234;
    d.cache_mb = 512;
    d.speed = 2.0f;
    d.time_mode = 2;
    d.still_duration_frames = 900;

    d.layers[0].stack.push_back(make_effect(d, EffectType::Vignette));
    d.layers[0].stack.push_back(make_effect(d, EffectType::Datamosh));
    d.layers[0].stack.push_back(make_effect(d, EffectType::ColorScience));
    d.layers[0].stack[1].params[3] = 6.0f;
    d.layers[0].stack[1].wet = 0.8f;
    d.layers[0].stack[1].blend = doc::BlendMode::Screen;
    d.layers[0].stack[1].seed = 99;

    doc::Layer overlay;
    overlay.id = d.next_effect_id++;
    overlay.name = "noise";
    overlay.source = doc::LayerSourceKind::Noise;
    overlay.blend = doc::BlendMode::Multiply;
    overlay.opacity = 0.4f;
    overlay.color_a[0] = 0.9f;
    overlay.gen_scale = 3.0f;
    overlay.stack.push_back(make_effect(d, EffectType::Pixelate));
    d.layers.push_back(overlay);

    // Group the base layer's first two effects with a macro.
    doc::Group g = doc::make_group(d, "combo");
    g.folded = true;
    doc::MacroKnob knob;
    knob.name = "wreck";
    knob.value = 0.5f;
    knob.targets.push_back({d.layers[0].stack[1].id, 4, 0.0f, 0.6f,
                            doc::ResponseCurve::Exp});
    g.macros.push_back(knob);
    d.layers[0].stack[0].group_id = g.id;
    d.layers[0].stack[1].group_id = g.id;
    d.layers[0].groups.push_back(g);

    doc::Mask m;
    m.id = d.next_mask_id++;
    m.name = "shape";
    m.type = doc::MaskType::Luma;
    m.blur_px = 4.0f;
    m.invert = true;
    m.source_path = "C:/clips/maskloop.mez";
    m.free_run = true;
    m.fit = 2;
    m.chain.push_back(make_effect(d, EffectType::Pixelate));
    d.masks.push_back(m);
    d.layers[0].stack[0].mask_id = m.id;

    doc::ModRoute r;
    r.id = d.next_route_id++;
    r.source.type = doc::ModSourceType::Lfo;
    r.source.shape = doc::LfoShape::Triangle;
    r.source.rate_hz = 2.0f;
    r.target = {d.layers[0].stack[0].id, 0};
    r.amount = 0.3f;
    r.curve = doc::ResponseCurve::SCurve;
    d.mod_routes.push_back(r);

    doc::ModRoute env;
    env.id = d.next_route_id++;
    env.source.type = doc::ModSourceType::Envelope;
    env.source.attack = 0.05f;
    env.source.decay = 0.8f;
    env.source.trigger = 1;
    env.target = {d.layers[0].stack[0].id, 1};
    env.amount = -0.5f;
    d.mod_routes.push_back(env);

    doc::KeyframeLane lane;
    lane.target = {d.layers[0].stack[2].id, 3};
    lane.keys.push_back({0.0, 0.0f, 4.0f, 0.1f, 0.0f, 0.0f, false});
    lane.keys.push_back({48.0, 1.0f, 0.0f, 0.0f, -4.0f, -0.1f, true});
    d.lanes.push_back(lane);

    d.snapshots[1].valid = true;
    d.snapshots[1].entries.push_back(
        {d.layers[0].stack[0].id, {0.5f, 0.5f, 0.2f}, 0.9f, 1.0f});
    return d;
}

}  // namespace

TEST(serialize_roundtrip_stable) {
    Document d = make_rich_doc();
    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    json::Value b = doc::doc_to_json(d2);
    CHECK(a == b);   // byte-stable roundtrip

    // Spot checks through the loaded document.
    CHECK_EQ(d2.name, "rich");
    CHECK_EQ(d2.clip_path, "C:/clips/test.mp4");
    CHECK_EQ(d2.master_seed, uint64_t{1234});
    CHECK_EQ(d2.cache_mb, uint32_t{512});
    CHECK_EQ(d2.still_duration_frames, uint32_t{900});
    CHECK_EQ(d2.speed, 2.0f);
    CHECK_EQ(d2.time_mode, uint32_t{2});
    CHECK_EQ(d2.layers.size(), size_t{2});
    CHECK_EQ(d2.layers[0].stack.size(), size_t{3});
    CHECK(d2.layers[0].stack[1].type == EffectType::Datamosh);
    CHECK_EQ(d2.layers[0].stack[1].wet, 0.8f);
    CHECK(d2.layers[0].stack[1].blend == doc::BlendMode::Screen);
    CHECK_EQ(d2.layers[0].stack[1].seed, uint64_t{99});
    CHECK(d2.layers[1].source == doc::LayerSourceKind::Noise);
    CHECK_EQ(d2.layers[1].opacity, 0.4f);
    CHECK_EQ(d2.layers[0].groups.size(), size_t{1});
    CHECK(d2.layers[0].groups[0].folded);
    CHECK_EQ(d2.layers[0].groups[0].macros.size(), size_t{1});
    CHECK_EQ(d2.layers[0].groups[0].macros[0].targets.size(), size_t{1});
    CHECK(d2.layers[0].groups[0].macros[0].targets[0].curve ==
          doc::ResponseCurve::Exp);
    CHECK_EQ(d2.layers[0].stack[0].group_id, d2.layers[0].groups[0].id);
    CHECK_EQ(d2.masks.size(), size_t{1});
    CHECK(d2.masks[0].invert);
    CHECK_EQ(d2.masks[0].source_path, "C:/clips/maskloop.mez");
    CHECK(d2.masks[0].free_run);
    CHECK_EQ(d2.masks[0].fit, 2u);
    CHECK_EQ(d2.masks[0].chain.size(), size_t{1});
    CHECK_EQ(d2.mod_routes.size(), size_t{2});
    CHECK(d2.mod_routes[0].source.shape == doc::LfoShape::Triangle);
    CHECK(d2.mod_routes[1].source.type == doc::ModSourceType::Envelope);
    CHECK_EQ(d2.mod_routes[1].source.attack, 0.05f);
    CHECK_EQ(d2.mod_routes[1].source.decay, 0.8f);
    CHECK_EQ(d2.mod_routes[1].source.trigger, uint32_t{1});
    CHECK_EQ(d2.lanes.size(), size_t{1});
    CHECK_EQ(d2.lanes[0].keys.size(), size_t{2});
    CHECK(d2.lanes[0].keys[1].hold);
    CHECK(d2.snapshots[1].valid);
    CHECK(!d2.snapshots[0].valid);
    // Counters stay usable.
    CHECK(d2.next_effect_id >= d.next_effect_id);
    CHECK(d2.next_mask_id >= d.next_mask_id);
}

TEST(serialize_tolerant_load) {
    // Unknown effect type skipped, missing keys default, counters re-derived
    // above the highest id even when the stored counters lie.
    const char* text = R"({
        "looks_project": 1,
        "name": "sparse",
        "next_effect_id": 1,
        "layers": [
            {"id": 40, "stack": [
                {"type": "from_the_future", "id": 41},
                {"type": "vignette", "id": 42, "params": [0.9]}
            ]}
        ]
    })";
    json::ParseResult parsed = json::parse(text);
    CHECK(parsed.value.has_value());
    Document d = doc::doc_from_json(*parsed.value);
    CHECK_EQ(d.layers.size(), size_t{1});
    CHECK_EQ(d.layers[0].stack.size(), size_t{1});   // unknown skipped
    CHECK(d.layers[0].stack[0].type == EffectType::Vignette);
    CHECK_EQ(d.layers[0].stack[0].params[0], 0.9f);
    // Missing params take defaults.
    CHECK_EQ(d.layers[0].stack[0].params[1],
             doc::effect_info(EffectType::Vignette).params[1].default_value);
    CHECK(d.layers[0].visible);
    CHECK_EQ(d.layers[0].opacity, 1.0f);
    CHECK(d.next_effect_id > 42);   // not the stored 1

    // Garbage text fails cleanly.
    CHECK(!json::parse("{nope").value.has_value());
}

TEST(group_commands_lifecycle) {
    Document d;
    doc::UndoStack undo;
    d.layers[0].stack.push_back(make_effect(d, EffectType::Vignette));
    d.layers[0].stack.push_back(make_effect(d, EffectType::Grain));
    d.layers[0].stack.push_back(make_effect(d, EffectType::Pixelate));

    doc::Group g = doc::make_group(d, "duo");
    undo.execute(d, doc::group_effects_command(0, g, 0, 1));
    CHECK_EQ(d.layers[0].groups.size(), size_t{1});
    CHECK_EQ(d.layers[0].stack[0].group_id, g.id);
    CHECK_EQ(d.layers[0].stack[1].group_id, g.id);
    CHECK_EQ(d.layers[0].stack[2].group_id, uint64_t{0});

    // Macro add + coalescing value drags.
    doc::MacroKnob knob;
    knob.name = "wear";
    knob.targets.push_back({d.layers[0].stack[1].id, 0, 0.0f, 0.5f,
                            doc::ResponseCurve::Linear});
    undo.execute(d, doc::add_macro_command(0, g.id, knob));
    CHECK_EQ(d.layers[0].groups[0].macros.size(), size_t{1});

    doc::Group edited = d.layers[0].groups[0];
    edited.macros[0].value = 0.4f;
    undo.execute(d, doc::set_group_props_command(0, edited), true);
    edited.macros[0].value = 0.8f;
    undo.execute(d, doc::set_group_props_command(0, edited), true);
    CHECK_EQ(d.layers[0].groups[0].macros[0].value, 0.8f);
    undo.undo(d);   // coalesced drag: one step back to 0
    CHECK_EQ(d.layers[0].groups[0].macros[0].value, 0.0f);
    undo.redo(d);

    undo.execute(d, doc::ungroup_command(0, g.id));
    CHECK(d.layers[0].groups.empty());
    CHECK_EQ(d.layers[0].stack[0].group_id, uint64_t{0});
    undo.undo(d);
    CHECK_EQ(d.layers[0].groups.size(), size_t{1});
    CHECK_EQ(d.layers[0].groups[0].macros[0].value, 0.8f);
    CHECK_EQ(d.layers[0].stack[1].group_id, g.id);
}

TEST(group_bypass_compiles_out) {
    Document d;
    d.layers[0].stack.push_back(make_effect(d, EffectType::Vignette));
    d.layers[0].stack.push_back(make_effect(d, EffectType::Pixelate));
    doc::Group g = doc::make_group(d, "off");
    g.bypass = true;
    d.layers[0].stack[0].group_id = g.id;
    d.layers[0].groups.push_back(g);

    gfx::RenderGraph graph = gfx::compile_graph(d);
    CHECK(graph.valid);
    // Source + pixelate only; the grouped vignette is compiled out.
    CHECK_EQ(graph.nodes.size(), size_t{2});
    CHECK_EQ(graph.nodes[1].effect_index, 1);
}

TEST(macro_eval_applies_offsets) {
    Document d;
    d.layers[0].stack.push_back(make_effect(d, EffectType::Vignette));
    const uint64_t fx_id = d.layers[0].stack[0].id;
    d.layers[0].stack[0].params[0] = 0.2f;   // amount, range 0..1

    doc::Group g = doc::make_group(d, "m");
    doc::MacroKnob knob;
    knob.name = "k";
    knob.value = 0.5f;
    knob.targets.push_back({fx_id, 0, 0.0f, 0.4f, doc::ResponseCurve::Linear});
    g.macros.push_back(knob);
    d.layers[0].stack[0].group_id = g.id;
    d.layers[0].groups.push_back(g);

    Document r = mod::resolve(d, 0, 30.0, nullptr);
    // 0.2 + lerp(0, 0.4, 0.5) * span(1.0) = 0.4
    CHECK(std::fabs(r.layers[0].stack[0].params[0] - 0.4f) < 1e-5f);

    // Knob at zero with lo=0 is neutral.
    d.layers[0].groups[0].macros[0].value = 0.0f;
    r = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(std::fabs(r.layers[0].stack[0].params[0] - 0.2f) < 1e-5f);

    // Clamped at the param ceiling.
    d.layers[0].groups[0].macros[0].value = 1.0f;
    d.layers[0].groups[0].macros[0].targets[0].hi = 4.0f;
    r = mod::resolve(d, 0, 30.0, nullptr);
    CHECK_EQ(r.layers[0].stack[0].params[0], 1.0f);
}

TEST(preset_capture_and_instantiate) {
    Document d;
    doc::UndoStack undo;
    d.layers[0].stack.push_back(make_effect(d, EffectType::ColorScience));
    d.layers[0].stack.push_back(make_effect(d, EffectType::Grain));
    d.layers[0].stack[0].params[3] = 0.3f;
    d.layers[0].stack[0].mask_id = 77;   // must not leak into the preset

    doc::Group g = doc::make_group(d, "era");
    doc::MacroKnob knob;
    knob.name = "fade";
    knob.targets.push_back({d.layers[0].stack[0].id, 3, 0.0f, 0.5f,
                            doc::ResponseCurve::Linear});
    // A dangling target (no such member) must be dropped on capture.
    knob.targets.push_back({9999, 0, 0.0f, 0.5f, doc::ResponseCurve::Linear});
    g.macros.push_back(knob);
    d.layers[0].stack[0].group_id = g.id;
    d.layers[0].stack[1].group_id = g.id;
    d.layers[0].groups.push_back(g);

    doc::Preset p = doc::make_preset_from_group(d, 0, g.id);
    CHECK_EQ(p.name, "era");
    CHECK_EQ(p.effects.size(), size_t{2});
    CHECK_EQ(p.effects[0].mask_id, uint64_t{0});
    CHECK_EQ(p.group.macros[0].targets.size(), size_t{1});

    // File roundtrip.
    json::Value pj = doc::preset_to_json(p);
    auto p2 = doc::preset_from_json(pj);
    CHECK(p2.has_value());
    CHECK(doc::preset_to_json(*p2) == pj);

    // Instantiate into a fresh doc: fresh ids, remapped macro targets.
    Document target;
    doc::Group ng;
    std::vector<doc::EffectInstance> nfx;
    doc::instantiate_preset(target, *p2, &ng, &nfx);
    CHECK_EQ(nfx.size(), size_t{2});
    CHECK(nfx[0].id != p2->effects[0].id);
    CHECK_EQ(nfx[0].group_id, ng.id);
    CHECK_EQ(ng.macros[0].targets.size(), size_t{1});
    CHECK_EQ(ng.macros[0].targets[0].effect_id, nfx[0].id);
    CHECK(ng.folded);

    undo.execute(target, doc::insert_group_command(0, ng, nfx));
    CHECK_EQ(target.layers[0].stack.size(), size_t{2});
    CHECK_EQ(target.layers[0].groups.size(), size_t{1});
    undo.undo(target);
    CHECK(target.layers[0].stack.empty());
    CHECK(target.layers[0].groups.empty());
    undo.redo(target);
    CHECK_EQ(target.layers[0].stack.size(), size_t{2});
}

TEST(morph_interpolates_snapshots) {
    Document d;
    doc::UndoStack undo;
    d.layers[0].stack.push_back(make_effect(d, EffectType::Vignette));

    d.layers[0].stack[0].params[0] = 0.0f;
    undo.execute(d, doc::store_snapshot_command(0));
    d.layers[0].stack[0].params[0] = 1.0f;
    undo.execute(d, doc::store_snapshot_command(1));
    d.layers[0].stack[0].params[0] = 0.5f;   // overridden while morphing

    undo.execute(d, doc::set_morph_command(0, 1, 0.25f));
    Document r = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(std::fabs(r.layers[0].stack[0].params[0] - 0.25f) < 1e-5f);

    // The morph position is itself a mod target ({0, 0}): a sine LFO at
    // t = 0 contributes 0.5 -> pos 0.75.
    doc::ModRoute route;
    route.id = d.next_route_id++;
    route.target = {0, 0};
    route.amount = 1.0f;
    d.mod_routes.push_back(route);
    r = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(std::fabs(r.layers[0].stack[0].params[0] - 0.75f) < 1e-4f);

    // Undo the morph command; project files carry morph state.
    undo.undo(d);
    CHECK_EQ(d.morph_pos, 0.0f);
    undo.redo(d);
    json::Value j = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(j);
    CHECK_EQ(d2.morph_pos, 0.25f);
    CHECK_EQ(d2.morph_to, 1);
    CHECK(doc::doc_to_json(d2) == j);
}

TEST(era_presets_ship_valid) {
    // The five spec §14 era presets in assets/presets must load, carry
    // effects, and have every macro target resolve to a member effect.
    const std::filesystem::path dir =
        std::filesystem::path(LOOKS_REPO_ROOT) / "assets" / "presets";
    std::vector<doc::Preset> presets = doc::scan_presets(dir);
    CHECK_EQ(presets.size(), size_t{5});
    for (const doc::Preset& p : presets) {
        CHECK(!p.effects.empty());
        CHECK(!p.tags.empty());
        for (const doc::MacroKnob& knob : p.group.macros)
            for (const doc::MacroTarget& t : knob.targets) {
                bool found = false;
                for (const doc::EffectInstance& fx : p.effects) {
                    if (fx.id != t.effect_id) continue;
                    found = true;
                    // Param index must exist on the target effect.
                    CHECK(t.param_index <
                          static_cast<int>(
                              doc::effect_info(fx.type).param_count));
                }
                CHECK(found);
            }
    }
}

TEST(serialize_mask_param_key_roundtrip) {
    // Mask target keys carry bit 63 — past the JSON number's 2^53 exact
    // range, so the mask id gets its own field. A route and a point lane
    // must survive save/load with the id intact.
    Document d;
    doc::Mask mask;
    mask.id = 3;
    mask.name = "m";
    mask.points = {0.2f, 0.2f, 0.8f, 0.2f, 0.5f, 0.8f};
    d.masks.push_back(mask);
    d.next_mask_id = 4;

    doc::ModRoute route;
    route.id = d.next_route_id++;
    route.target = {mask.id | doc::kMaskParamBit, 0};
    route.amount = 0.4f;
    d.mod_routes.push_back(route);

    doc::KeyframeLane lane;
    lane.target = {mask.id | doc::kMaskParamBit,
                   doc::kMaskPointParamBase + 2};
    lane.keys = {{0.0, 0.2f}, {12.0, 0.7f}};
    d.lanes.push_back(lane);

    json::Value j = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(j);
    CHECK_EQ(d2.mod_routes.size(), size_t{1});
    CHECK_EQ(d2.mod_routes[0].target.effect_id,
             mask.id | doc::kMaskParamBit);
    CHECK_EQ(d2.lanes.size(), size_t{1});
    CHECK_EQ(d2.lanes[0].target.effect_id, mask.id | doc::kMaskParamBit);
    CHECK_EQ(d2.lanes[0].target.param_index, doc::kMaskPointParamBase + 2);
    CHECK(doc::doc_to_json(d2) == j);
}
