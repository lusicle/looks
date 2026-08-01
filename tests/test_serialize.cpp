#include "doc/serialize.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/mod_commands.h"
#include "doc/preset.h"
#include "doc/stack_commands.h"
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
    d.layers[0].stack.push_back(make_effect(d, EffectType::FilmStock));
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
    // The one string param: Text's string must survive the trip.
    overlay.stack.push_back(make_effect(d, EffectType::Text));
    overlay.stack.back().text = "REC · SP";
    d.layers.push_back(overlay);

    // Group the base layer's first two effects; expose a member param
    // on the face (the face is direct param aliases).
    doc::Group g = doc::make_group(d, "combo");
    g.folded = true;
    g.exposed.push_back({d.layers[0].stack[1].id, 4});
    d.layers[0].stack[0].group_id = g.id;
    d.layers[0].stack[1].group_id = g.id;
    d.layers[0].groups.push_back(g);

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
    CHECK_EQ(d2.layers[0].groups[0].exposed.size(), size_t{1});
    CHECK_EQ(d2.layers[0].groups[0].exposed[0].param_index, 4);
    CHECK_EQ(d2.layers[0].stack[0].group_id, d2.layers[0].groups[0].id);
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

TEST(serialize_v57_roundtrip) {
    // Markers, export settings, layer-param lanes — byte-stable.
    Document d;
    d.markers = {12, 45, 90};
    d.export_bitrate_mbps = 22.0f;
    d.export_scale = 2;
    d.export_audio = false;
    doc::KeyframeLane lane;
    lane.target = {d.layers[0].id | doc::kLayerParamBit, 8};
    lane.keys.push_back({0.0, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    lane.keys.push_back({30.0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    d.lanes.push_back(lane);

    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    CHECK_EQ(d2.markers.size(), size_t{3});
    CHECK_EQ(d2.markers[1], uint32_t{45});
    CHECK_EQ(d2.export_bitrate_mbps, 22.0f);
    CHECK_EQ(d2.export_scale, uint32_t{2});
    CHECK(!d2.export_audio);
    CHECK_EQ(d2.lanes.size(), size_t{1});
    CHECK_EQ(d2.lanes[0].target.effect_id,
             d.layers[0].id | doc::kLayerParamBit);
    CHECK_EQ(d2.lanes[0].target.param_index, 8);
    json::Value b = doc::doc_to_json(d2);
    CHECK(a == b);
}

TEST(preset_insert_lands_dormant) {
    // v5.8: adding never wires. The preset's members chain INTERNALLY
    // only; the group card joins the graph through a wire gesture. The
    // pre-existing chain wiring freezes as-is.
    Document d;
    d.layers[0].stack.push_back(make_effect(d, EffectType::Vignette));
    doc::UndoStack undo;

    doc::Group g;
    g.id = d.next_effect_id++;
    g.name = "preset";
    std::vector<doc::EffectInstance> members;
    members.push_back(make_effect(d, EffectType::Grain));
    members.push_back(make_effect(d, EffectType::Posterize));
    const uint64_t m0 = members[0].id, m1 = members[1].id;
    undo.execute(d, doc::insert_group_command(0, g, std::move(members)));

    // The old chain froze: source -> vignette -> output.
    CHECK(!d.links.empty());
    bool internal = false, boundary = false, chain_out = false;
    for (const Document::NodeLink& l : d.links) {
        if (l.from == m0 && l.to == m1 && l.to_port == 0) internal = true;
        if ((l.from == m1 || l.from == m0) && l.to == 0) boundary = true;
        if (l.to == m0) boundary = true;   // nothing feeds the group
        if (l.to == 0 && l.from == d.layers[0].stack[0].id)
            chain_out = true;
    }
    CHECK(internal);
    CHECK(!boundary);
    CHECK(chain_out);

    // Undo restores the un-materialized document exactly.
    undo.undo(d);
    CHECK(d.links.empty());
    CHECK_EQ(d.layers[0].groups.size(), size_t{0});
    CHECK_EQ(d.layers[0].stack.size(), size_t{1});
}

TEST(serialize_dedupes_input_fanin) {
    // In ports hold ONE producer; only the Output composites
    // fan-in. Hand-edited files keep the LAST link per (to, port).
    const char* text = R"({
        "looks_project": 1,
        "layers": [{"id": 1, "stack": [
            {"type": "vignette", "id": 2},
            {"type": "grain", "id": 3}
        ]}],
        "links": [
            {"from": 1, "to": 2, "port": 0},
            {"from": 3, "to": 2, "port": 0},
            {"from": 1, "to": 0, "port": 0},
            {"from": 2, "to": 0, "port": 0}
        ]
    })";
    json::ParseResult parsed = json::parse(text);
    CHECK(parsed.value.has_value());
    Document d = doc::doc_from_json(*parsed.value);
    int into_effect2 = 0, into_output = 0;
    uint64_t producer = 0;
    for (const Document::NodeLink& l : d.links) {
        if (l.to == 2 && l.to_port == 0) {
            ++into_effect2;
            producer = l.from;
        }
        if (l.to == 0) ++into_output;
    }
    CHECK_EQ(into_effect2, 1);
    CHECK_EQ(producer, uint64_t{3});   // the LAST one wins
    CHECK_EQ(into_output, 2);          // the Output merge keeps fan-in
}

TEST(serialize_lane_keys_sorted_on_load) {
    // Hand-authored files may list keys out of order; eval assumes sorted
    // — the loader must sort.
    const char* text = R"({
        "looks_project": 1,
        "layers": [{"id": 1, "stack": [{"type": "dither", "id": 2}]}],
        "lanes": [{"target": {"effect": 2, "param": 0},
                   "keys": [{"frame": 60, "value": 8},
                            {"frame": 0, "value": 2},
                            {"frame": 30, "value": 16}]}]
    })";
    json::ParseResult parsed = json::parse(text);
    CHECK(parsed.value.has_value());
    Document d = doc::doc_from_json(*parsed.value);
    CHECK_EQ(d.lanes.size(), size_t{1});
    CHECK_EQ(d.lanes[0].keys.size(), size_t{3});
    CHECK_EQ(d.lanes[0].keys[0].frame, 0.0);
    CHECK_EQ(d.lanes[0].keys[1].frame, 30.0);
    CHECK_EQ(d.lanes[0].keys[2].frame, 60.0);
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

    // Face exposure: expose, de-dup, hide, undo both ways.
    const doc::ParamKey pk{d.layers[0].stack[1].id, 0};
    undo.execute(d, doc::set_group_exposed_command(0, g.id, pk, true));
    CHECK_EQ(d.layers[0].groups[0].exposed.size(), size_t{1});
    undo.execute(d, doc::set_group_exposed_command(0, g.id, pk, true));
    CHECK_EQ(d.layers[0].groups[0].exposed.size(), size_t{1});   // no dup
    undo.execute(d, doc::set_group_exposed_command(0, g.id, pk, false));
    CHECK(d.layers[0].groups[0].exposed.empty());
    undo.undo(d);   // un-hide
    CHECK_EQ(d.layers[0].groups[0].exposed.size(), size_t{1});
    CHECK(d.layers[0].groups[0].exposed[0] == pk);

    undo.execute(d, doc::ungroup_command(0, g.id));
    CHECK(d.layers[0].groups.empty());
    CHECK_EQ(d.layers[0].stack[0].group_id, uint64_t{0});
    undo.undo(d);
    CHECK_EQ(d.layers[0].groups.size(), size_t{1});
    CHECK_EQ(d.layers[0].groups[0].exposed.size(), size_t{1});
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

TEST(preset_capture_and_instantiate) {
    Document d;
    doc::UndoStack undo;
    d.layers[0].stack.push_back(make_effect(d, EffectType::FilmStock));
    d.layers[0].stack.push_back(make_effect(d, EffectType::Grain));
    d.layers[0].stack[0].params[3] = 0.3f;

    doc::Group g = doc::make_group(d, "era");
    g.exposed.push_back({d.layers[0].stack[0].id, 3});
    // A dangling face key (no such member) must be dropped on capture.
    g.exposed.push_back({9999, 0});
    d.layers[0].stack[0].group_id = g.id;
    d.layers[0].stack[1].group_id = g.id;
    d.layers[0].groups.push_back(g);

    doc::Preset p = doc::make_preset_from_group(d, 0, g.id);
    CHECK_EQ(p.name, "era");
    CHECK_EQ(p.effects.size(), size_t{2});
    CHECK_EQ(p.group.exposed.size(), size_t{1});

    // File roundtrip.
    json::Value pj = doc::preset_to_json(p);
    auto p2 = doc::preset_from_json(pj);
    CHECK(p2.has_value());
    CHECK(doc::preset_to_json(*p2) == pj);

    // Instantiate into a fresh doc: fresh ids, remapped face keys.
    Document target;
    doc::Group ng;
    std::vector<doc::EffectInstance> nfx;
    doc::instantiate_preset(target, *p2, &ng, &nfx);
    CHECK_EQ(nfx.size(), size_t{2});
    CHECK(nfx[0].id != p2->effects[0].id);
    CHECK_EQ(nfx[0].group_id, ng.id);
    CHECK_EQ(ng.exposed.size(), size_t{1});
    CHECK_EQ(ng.exposed[0].effect_id, nfx[0].id);
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
    // The shipped presets in assets/presets (the five era looks
    // plus the wave-2 style set) must load, carry effects, and have every
    // exposed face param resolve to a member effect (the group
    // face is exposed params — direct aliases, no macro offsets).
    const std::filesystem::path dir =
        std::filesystem::path(LOOKS_REPO_ROOT) / "assets" / "presets";
    std::vector<doc::Preset> presets = doc::scan_presets(dir);
    CHECK_EQ(presets.size(), size_t{15});
    for (const doc::Preset& p : presets) {
        CHECK(!p.effects.empty());
        CHECK(!p.tags.empty());
        for (const doc::ParamKey& k : p.group.exposed) {
            bool found = false;
            for (const doc::EffectInstance& fx : p.effects) {
                if (fx.id != k.effect_id) continue;
                found = true;
                CHECK(k.param_index <
                      static_cast<int>(
                          doc::effect_info(fx.type).param_count));
                CHECK(k.param_index >= doc::kOpacityParam);
            }
            CHECK(found);
        }
    }
}


