#include "doc/serialize.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/look_commands.h"
#include "doc/mod_commands.h"
#include "doc/preset.h"
#include "doc/stack_commands.h"
#include "gfx/graph.h"
#include "mod/eval.h"
#include "doc_fixture.h"
#include "test_framework.h"

using namespace looks;

TEST(connection_blends_survive_storage_presets_and_group_edits) {
    auto d = doc_with_look();
    auto& look = d.looks[0];
    look.sources[0].source = doc::SourceKind::Solid;
    look.sources.push_back(doc::make_source(d, doc::SourceKind::Noise));
    look.effects.push_back(doc::make_effect(d, doc::EffectType::Invert));
    const auto a = look.sources[0].id, b = look.sources[1].id, fx = look.effects[0].id;
    look.links = {{a, fx, 0}, {b, fx, 0, doc::BlendMode::Multiply},
        {fx, 0, 0, doc::BlendMode::Screen}, {a, 0, 1}, {b, 0, 1}};
    look.audio_split = true;
    const auto original = look.links;
    auto text = json::parse(json::write(doc::doc_to_json(d)));
    CHECK(text.value.has_value());
    auto loaded = doc::doc_from_json(*text.value);
    CHECK(loaded.looks[0].links == original);
    auto group = doc::make_group(d, "blend");
    doc::UndoStack undo;
    undo.execute(d, doc::group_effects_command(look.id, group, {fx}));
    auto preset = doc::make_preset_from_group(look, group.id);
    auto parsed = doc::preset_from_json(doc::preset_to_json(preset));
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->links == preset.links);
        auto instance = doc::instantiate_preset(d, *parsed);
        CHECK_EQ(instance.links.size(), preset.links.size());
        for (size_t i = 0; i < preset.links.size(); ++i) {
            CHECK(instance.links[i].blend == preset.links[i].blend);
            CHECK(instance.links[i].from != preset.links[i].from);
        }
    }
    undo.execute(d, doc::ungroup_command(look.id, group.id));
    CHECK(look.links == original);
    undo.undo(d);
    const auto slot = look.groups[0].inputs[0];
    undo.execute(d, doc::connect_command(look.id, {b, slot, 0, doc::BlendMode::Difference}));
    const auto grouped = look.links;
    undo.execute(d, doc::ungroup_command(look.id, group.id));
    CHECK_EQ(look.effects.size(), size_t{2});
    CHECK(look.effects.back().bypass);
    CHECK(look.effects.back().type == doc::EffectType::BlendNode);
    CHECK(doc::valid_look_graph(look));
    const auto ungrouped = look.links;
    const auto merge_id = look.effects.back().id;
    undo.undo(d);
    CHECK(look.links == grouped);
    CHECK_EQ(look.effects.size(), size_t{1});
    undo.redo(d);
    CHECK(look.links == ungrouped);
    CHECK_EQ(look.effects.back().id, merge_id);
    undo.undo(d);
    undo.execute(d, doc::set_effect_group_command(look.id, fx, 0));
    CHECK_EQ(doc::find_effect(look, fx)->group_id, uint64_t{0});
    CHECK_EQ(look.effects.size(), size_t{2});
    CHECK(look.effects.back().bypass);
    CHECK(doc::valid_look_graph(look));
    const auto moved = look.links;
    undo.undo(d);
    CHECK(look.links == grouped);
    CHECK_EQ(look.effects.size(), size_t{1});
    undo.redo(d);
    CHECK(look.links == moved);
    auto invalid = doc::link_to_json(original[0]);
    invalid.set("blend", "invalid");
    CHECK(doc::link_from_json(invalid).blend == doc::BlendMode::Count);
}

TEST(independent_graph_validation_rejects_ambiguous_ids_and_bad_bindings) {
    auto d = doc_with_look();
    auto& look = d.looks[0];
    CHECK(doc::valid_look_graph(look));
    auto effect = doc::make_effect(d, doc::EffectType::Invert);
    look.effects.push_back(effect);
    look.effects[0].id = look.sources[0].id;
    CHECK(!doc::valid_look_graph(look));
    look.effects[0].id = effect.id;
    look.effects[0].group_id = 99999;
    CHECK(!doc::valid_look_graph(look));
    look.effects[0].group_id = 0;
    look.links.push_back({99999, effect.id, 0});
    CHECK(!doc::valid_look_graph(look));
    look.links.back() = {look.sources[0].id, effect.id, 50};
    CHECK(!doc::valid_look_graph(look));
    look.links.back() = {look.sources[0].id, effect.id, 0};
    CHECK(doc::valid_look_graph(look));
    auto group = doc::make_group(d, "invalid face");
    look.groups.push_back(group);
    look.groups[0].face_out = effect.id;
    CHECK(!doc::valid_look_graph(look));
    look.effects[0].group_id = group.id;
    CHECK(doc::valid_look_graph(look));
    look.groups[0].exposed.push_back({effect.id, 10000});
    CHECK(!doc::valid_look_graph(look));
}

TEST(parameter_tags_roundtrip_through_json_text_without_losing_ids) {
    auto d = doc_with_look();
    const std::vector<uint64_t> ids{7 | doc::kSourceParamBit, 13 | doc::kStopParamBit,
        17 | doc::kGroupParamBit, 23};
    for (const auto id : ids) {
        d.looks[0].lanes.push_back({{id, 0}, {{0, 0.5f}}});
        doc::ModRoute route;
        route.target = {id, 0};
        d.looks[0].mod_routes.push_back(route);
    }
    const auto parsed = json::parse(json::write(doc::doc_to_json(d)));
    CHECK(parsed.value.has_value());
    if (!parsed.value) return;
    const auto copy = doc::doc_from_json(*parsed.value);
    for (size_t i = 0; i < ids.size(); ++i) {
        CHECK_EQ(copy.looks[0].lanes[i].target.effect_id, ids[i]);
        CHECK_EQ(copy.looks[0].mod_routes[i].target.effect_id, ids[i]);
    }
}

TEST(composition_crop_resize_and_storage) {
    auto d = doc_with_look();
    d.canvas_w = 1920;
    d.canvas_h = 1080;
    const auto id = d.looks[0].id;
    doc::UndoStack undo;
    undo.execute(d, doc::set_entity_format_command(id,
        doc::resize_format(d, id, 854, 480, true)));
    undo.execute(d, doc::set_entity_format_command(id,
        doc::resize_format(d, id, 480, 480, false)));
    auto f = doc::entity_format(d, id);
    CHECK_EQ(f.w, 480u);
    CHECK_EQ(f.content_w, 1920.0);
    CHECK_EQ(f.scale_x, 854.0 / 1920.0);
    CHECK_EQ(f.origin_x, 187.0);
    const auto restored = doc::doc_from_json(doc::doc_to_json(d));
    CHECK_EQ(doc::entity_format(restored, id).origin_x, 187.0);
    CHECK_EQ(doc::entity_format(restored, id).scale_x, f.scale_x);
    undo.undo(d);
    f = doc::entity_format(d, id);
    CHECK_EQ(f.w, 854u);
    CHECK_EQ(f.origin_x, 0.0);
    undo.redo(d);
    undo.execute(d, doc::set_entity_format_command(id,
        doc::resize_format(d, id, 854, 480, false)));
    CHECK_EQ(doc::entity_format(d, id).origin_x, 0.0);
    CHECK_EQ(doc::entity_format(d, id).content_w, 1920.0);
}
using doc::Document;
using doc::EffectType;
using doc::make_effect;

namespace {

Document make_rich_doc() {
    Document d = doc_with_look();
    d.name = "rich";
    doc::Asset media;
    media.id = d.next_effect_id++;
    media.name = "test.mp4";
    media.path = "C:/clips/test.mp4";
    media.still_duration_frames = 900;
    media.still = true;
    media.frame_count = 1200;
    d.assets.push_back(media);
    d.looks[0].sources[0].asset = media.id;
    d.looks[0].sources[0].slip = 12;
    d.looks[0].format.fps = 24.0;
    d.sequences[0].format.w = 1280;
    d.sequences[0].format.h = 720;
    d.master_seed = 1234;
    d.cache_mb = 512;
    d.speed = 2.0f;
    d.time_mode = 2;

    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    d.looks[0].effects.push_back(make_effect(d, EffectType::Datamosh));
    d.looks[0].effects.push_back(make_effect(d, EffectType::FilmStock));
    d.looks[0].effects[1].params[3] = 6.0f;
    d.looks[0].effects[1].wet = 0.8f;
    d.looks[0].effects[1].blend = doc::BlendMode::Screen;
    d.looks[0].effects[1].seed = 99;

    doc::Source overlay;
    overlay.id = d.next_effect_id++;
    overlay.name = "noise";
    overlay.source = doc::SourceKind::Noise;
    overlay.opacity = 0.4f;
    overlay.color_a[0] = 0.9f;
    overlay.gen_scale = 3.0f;
    d.looks[0].effects.push_back(make_effect(d, EffectType::Pixelate));
    // The one string param: Text's string must survive the trip.
    d.looks[0].effects.push_back(make_effect(d, EffectType::Text));
    d.looks[0].effects.back().text = "REC · SP";
    d.looks[0].sources.push_back(overlay);

    doc::Group g = doc::make_group(d, "combo");
    g.folded = true;
    g.exposed.push_back({d.looks[0].effects[1].id, 4});
    d.looks[0].effects[0].group_id = g.id;
    d.looks[0].effects[1].group_id = g.id;
    d.looks[0].groups.push_back(g);

    doc::ValueNode lfo_node;
    lfo_node.id = d.next_route_id++;
    lfo_node.source.type = doc::ModSourceType::Lfo;
    lfo_node.source.shape = doc::LfoShape::Triangle;
    lfo_node.source.rate_hz = 2.0f;
    d.looks[0].value_nodes.push_back(lfo_node);

    doc::ValueNode env_node;
    env_node.id = d.next_route_id++;
    env_node.source.type = doc::ModSourceType::Envelope;
    env_node.source.attack = 0.05f;
    env_node.source.decay = 0.8f;
    env_node.source.trigger = 1;
    d.looks[0].value_nodes.push_back(env_node);

    doc::ValueNode math_node;
    math_node.id = d.next_route_id++;
    math_node.source.type = doc::ModSourceType::Math;
    math_node.op = doc::ValueOp::Multiply;
    math_node.in_a = lfo_node.id;
    math_node.const_b = 0.5f;
    d.looks[0].value_nodes.push_back(math_node);

    doc::ModRoute r;
    r.id = d.next_route_id++;
    r.node = math_node.id;
    r.target = {d.looks[0].effects[0].id, 0};
    r.curve = doc::ResponseCurve::SCurve;
    d.looks[0].mod_routes.push_back(r);

    doc::ModRoute env;
    env.id = d.next_route_id++;
    env.node = env_node.id;
    env.target = {d.looks[0].effects[0].id, 1};
    d.looks[0].mod_routes.push_back(env);

    doc::KeyframeLane lane;
    lane.target = {d.looks[0].effects[2].id, 3};
    lane.keys.push_back({0.0, 0.0f, 4.0f, 0.1f, 0.0f, 0.0f, false});
    lane.keys.push_back({48.0, 1.0f, 0.0f, 0.0f, -4.0f, -0.1f, true});
    d.looks[0].lanes.push_back(lane);

    d.looks[0].snapshots[1].valid = true;
    d.looks[0].snapshots[1].entries.push_back(
        {d.looks[0].effects[0].id, {0.5f, 0.5f, 0.2f}, 0.9f, 1.0f});

    const uint64_t pair = d.next_effect_id++;
    doc::Placement block;
    block.id = d.next_effect_id++;
    block.target = d.looks[0].id;
    block.t_in = 8;
    block.t_out = 120;
    block.source_in = 2;
    block.speed = 2.0f;
    block.link = pair;
    d.root().tracks[0].placements.push_back(block);
    doc::AudioTrack at;
    at.id = d.next_effect_id++;
    at.name = "a1";
    at.gain = 0.8f;
    doc::Placement ap;
    ap.id = d.next_effect_id++;
    ap.target = d.looks[0].id;
    ap.t_in = 8;
    ap.t_out = 120;
    ap.link = pair;
    ap.audio_gain = 0.5f;
    at.placements.push_back(ap);
    d.root().audio.clear();   // the fixture's a1 replaces the default
    d.root().audio.push_back(at);
    d.root().trim_in = 4;
    d.root().trim_out = 110;
    d.root().markers = {12, 45, 90};
    return d;
}

}  // namespace

TEST(serialize_placement_transform_roundtrip) {
    Document d = doc_with_look();
    doc::Placement p;
    p.id = d.next_effect_id++;
    p.target = d.looks[0].id;
    p.t_out = 50;
    p.pos_x = 0.25f;
    p.pos_y = -0.1f;
    p.scale = 0.5f;
    p.rotate = 45.0f;
    p.opacity = 0.7f;
    p.anchor_x = 0.2f;
    p.anchor_y = 0.8f;
    d.sequences[0].tracks[0].placements.push_back(p);
    // The anchor must persist even when the transform is otherwise identity.
    d.looks[0].sources[0].xf_anchor_x = 0.1f;
    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    CHECK(doc::doc_to_json(d2) == a);
    const doc::Placement* q = doc::find_placement(d2.sequences[0], p.id);
    CHECK(q != nullptr);
    CHECK_EQ(q->pos_x, 0.25f);
    CHECK_EQ(q->pos_y, -0.1f);
    CHECK_EQ(q->scale, 0.5f);
    CHECK_EQ(q->rotate, 45.0f);
    CHECK_EQ(q->opacity, 0.7f);
    CHECK_EQ(q->anchor_x, 0.2f);
    CHECK_EQ(q->anchor_y, 0.8f);
    CHECK_EQ(d2.looks[0].sources[0].xf_anchor_x, 0.1f);
    CHECK_EQ(d2.looks[0].sources[0].xf_anchor_y, 0.5f);
}

TEST(serialize_shape_path_roundtrip) {
    Document d = doc_with_look();
    doc::Source& l = d.looks[0].sources[0];
    l.source = doc::SourceKind::Shape;
    l.osc_shape = 3;
    doc::PathPoint p0, p1, p2;
    p0.ax = 0.5f;
    p0.ay = 0.2f;
    p0.out_dx = 0.1f;
    p0.out_dy = 0.05f;
    p1.ax = 0.8f;
    p1.ay = 0.8f;
    p1.in_dx = -0.03f;
    p2.ax = 0.2f;
    p2.ay = 0.8f;
    l.path = {p0, p1, p2};
    l.path_closed = false;
    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    CHECK(doc::doc_to_json(d2) == a);
    const doc::Source& l2 = d2.looks[0].sources[0];
    CHECK_EQ(l2.path.size(), size_t{3});
    CHECK_EQ(l2.path[0].ax, 0.5f);
    CHECK_EQ(l2.path[0].out_dx, 0.1f);
    CHECK_EQ(l2.path[0].out_dy, 0.05f);
    CHECK_EQ(l2.path[1].in_dx, -0.03f);
    CHECK_EQ(l2.path[2].ay, 0.8f);
    CHECK(!l2.path_closed);
    // Pathless layers stay pathless (and closed by default) on reload.
    Document d3 = doc_with_look();
    Document d4 = doc::doc_from_json(doc::doc_to_json(d3));
    CHECK(d4.looks[0].sources[0].path.empty());
    CHECK(d4.looks[0].sources[0].path_closed);
}

TEST(serialize_camera_node_roundtrip) {
    Document d = doc_with_look();
    doc::ValueNode n;
    n.id = d.next_route_id++;
    n.source.type = doc::ModSourceType::Camera;
    n.source.channel = 2;   // stab rot
    n.source.anchor = 417u; // locked 3D feature-track id
    n.audio_src = d.looks[0].sources[0].id;
    d.looks[0].value_nodes.push_back(n);
    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    CHECK(doc::doc_to_json(d2) == a);
    const doc::ValueNode* n2 = doc::find_value_node(d2.looks[0], n.id);
    CHECK(n2 != nullptr);
    CHECK(n2->source.type == doc::ModSourceType::Camera);
    CHECK_EQ(n2->source.channel, 2u);
    CHECK_EQ(n2->source.anchor, 417u);
    CHECK_EQ(n2->audio_src, d.looks[0].sources[0].id);
}

TEST(serialize_bins_roundtrip_and_heal) {
    Document d = doc_with_look();
    doc::Bin media = doc::make_bin(d, "media");
    doc::Bin cuts = doc::make_bin(d, "cuts");
    cuts.parent = media.id;
    d.bins.push_back(media);
    d.bins.push_back(cuts);
    d.looks[0].bin = cuts.id;
    d.sequences[0].bin = media.id;
    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    json::Value b = doc::doc_to_json(d2);
    CHECK(a == b);
    CHECK_EQ(d2.bins.size(), size_t{2});
    CHECK_EQ(d2.bins[1].parent, media.id);
    CHECK_EQ(d2.looks[0].bin, cuts.id);
    CHECK_EQ(d2.sequences[0].bin, media.id);

    // Heals: a dangling membership and a parent loop both fall to root.
    Document h = doc_with_look();
    doc::Bin a1 = doc::make_bin(h, "a");
    doc::Bin b1 = doc::make_bin(h, "b");
    a1.parent = b1.id;
    b1.parent = a1.id;
    h.bins.push_back(a1);
    h.bins.push_back(b1);
    h.looks[0].bin = 424242;
    Document h2 = doc::doc_from_json(doc::doc_to_json(h));
    CHECK_EQ(h2.looks[0].bin, uint64_t{0});
    bool any_root = false;
    for (const doc::Bin& bb : h2.bins)
        if (bb.parent == 0) any_root = true;
    CHECK(any_root);
}

TEST(serialize_audio_voice_roundtrip) {
    // Defaults stay absent from the JSON.
    Document d = doc_with_look();
    d.looks[0].audio_split = true;
    d.looks[0].sources[0].asset = d.next_effect_id++;
    d.looks[0].sources[0].timeline_lock = true;
    d.looks[0].effects.push_back(
        make_effect(d, EffectType::AudioFilter));
    d.looks[0].effects[0].params[0] = 0.25f;
    d.looks[0].effects.push_back(
        make_effect(d, EffectType::Offset));
    d.looks[0].effects[1].params[0] = -24.0f;
    d.looks[0].effects[1].params[1] = 1.0f;
    d.looks[0].links.push_back({d.looks[0].sources[0].id, 0, 1});
    doc::ValueNode vn;
    vn.id = d.next_route_id++;
    vn.source.type = doc::ModSourceType::AudioHigh;
    vn.audio_src = d.looks[0].sources[0].id;
    d.looks[0].value_nodes.push_back(vn);

    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    json::Value b = doc::doc_to_json(d2);
    CHECK(a == b);
    CHECK(d2.looks[0].audio_split);
    CHECK(d2.looks[0].sources[0].timeline_lock);
    CHECK(d2.looks[0].effects[0].type == EffectType::AudioFilter);
    CHECK_EQ(d2.looks[0].effects[0].params[0], 0.25f);
    CHECK(d2.looks[0].effects[1].type == EffectType::Offset);
    CHECK_EQ(doc::offset_frames(d2.looks[0].effects[1]),
             int64_t{-24});
    CHECK(doc::offset_targets_audio(d2.looks[0].effects[1]));
    CHECK(!doc::offset_targets_video(d2.looks[0].effects[1]));
    CHECK_EQ(d2.looks[0].links.back().to, uint64_t{0});
    CHECK_EQ(d2.looks[0].links.back().to_port, uint32_t{1});
    CHECK_EQ(d2.looks[0].value_nodes[0].audio_src,
             d2.looks[0].sources[0].id);

    Document plain = doc_with_look();
    CHECK(!doc::doc_from_json(doc::doc_to_json(plain)).looks[0].audio_split);
    CHECK(!doc::doc_from_json(doc::doc_to_json(plain))
               .looks[0]
               .sources[0]
               .timeline_lock);
}

TEST(serialize_roundtrip_stable) {
    Document d = make_rich_doc();
    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    json::Value b = doc::doc_to_json(d2);
    CHECK(a == b);

    CHECK_EQ(d2.name, "rich");
    CHECK_EQ(d2.assets.size(), size_t{1});
    CHECK_EQ(d2.assets[0].path, "C:/clips/test.mp4");
    CHECK_EQ(d2.assets[0].still_duration_frames, uint32_t{900});
    CHECK(d2.assets[0].still);
    CHECK_EQ(d2.looks[0].format.fps, 24.0);
    CHECK_EQ(d2.looks[0].format.w, uint32_t{0});
    CHECK_EQ(d2.sequences[0].format.w, uint32_t{1280});
    CHECK_EQ(d2.sequences[0].format.h, uint32_t{720});
    CHECK_EQ(d2.sequences[0].format.fps, 0.0);
    CHECK_EQ(d2.looks[0].sources[0].asset, d2.assets[0].id);
    CHECK_EQ(d2.looks[0].sources[0].slip, uint32_t{12});
    CHECK_EQ(d2.master_seed, uint64_t{1234});
    CHECK_EQ(d2.cache_mb, uint32_t{512});
    CHECK_EQ(d2.speed, 2.0f);
    CHECK_EQ(d2.time_mode, uint32_t{2});
    CHECK_EQ(d2.looks[0].sources.size(), size_t{2});
    CHECK_EQ(d2.looks[0].effects.size(), size_t{5});
    CHECK(d2.looks[0].effects[1].type == EffectType::Datamosh);
    CHECK_EQ(d2.looks[0].effects[1].wet, 0.8f);
    CHECK(d2.looks[0].effects[1].blend == doc::BlendMode::Screen);
    CHECK_EQ(d2.looks[0].effects[1].seed, uint64_t{99});
    CHECK(d2.looks[0].sources[1].source == doc::SourceKind::Noise);
    CHECK_EQ(d2.looks[0].sources[1].opacity, 0.4f);
    CHECK_EQ(d2.looks[0].groups.size(), size_t{1});
    CHECK(d2.looks[0].groups[0].folded);
    CHECK_EQ(d2.looks[0].groups[0].exposed.size(), size_t{1});
    CHECK_EQ(d2.looks[0].groups[0].exposed[0].param_index, 4);
    CHECK_EQ(d2.looks[0].effects[0].group_id,
             d2.looks[0].groups[0].id);
    CHECK_EQ(d2.looks[0].value_nodes.size(), size_t{3});
    CHECK(d2.looks[0].value_nodes[0].source.shape ==
          doc::LfoShape::Triangle);
    CHECK(d2.looks[0].value_nodes[1].source.type ==
          doc::ModSourceType::Envelope);
    CHECK_EQ(d2.looks[0].value_nodes[1].source.attack, 0.05f);
    CHECK_EQ(d2.looks[0].value_nodes[1].source.decay, 0.8f);
    CHECK_EQ(d2.looks[0].value_nodes[1].source.trigger, uint32_t{1});
    CHECK(d2.looks[0].value_nodes[2].source.type ==
          doc::ModSourceType::Math);
    CHECK(d2.looks[0].value_nodes[2].op == doc::ValueOp::Multiply);
    CHECK_EQ(d2.looks[0].value_nodes[2].in_a,
             d2.looks[0].value_nodes[0].id);
    CHECK_EQ(d2.looks[0].value_nodes[2].const_b, 0.5f);
    CHECK_EQ(d2.looks[0].mod_routes.size(), size_t{2});
    CHECK_EQ(d2.looks[0].mod_routes[0].node,
             d2.looks[0].value_nodes[2].id);
    CHECK(d2.looks[0].mod_routes[0].curve == doc::ResponseCurve::SCurve);
    CHECK_EQ(d2.looks[0].mod_routes[1].node,
             d2.looks[0].value_nodes[1].id);
    CHECK_EQ(d2.looks[0].lanes.size(), size_t{1});
    CHECK_EQ(d2.looks[0].lanes[0].keys.size(), size_t{2});
    CHECK(d2.looks[0].lanes[0].keys[1].hold);
    CHECK(d2.looks[0].snapshots[1].valid);
    CHECK(!d2.looks[0].snapshots[0].valid);
    CHECK_EQ(d2.root().tracks.size(), size_t{1});
    CHECK_EQ(d2.root().tracks[0].placements.size(), size_t{1});
    const doc::Placement& blk = d2.root().tracks[0].placements[0];
    CHECK_EQ(blk.target, d2.looks[0].id);
    CHECK_EQ(blk.t_in, uint32_t{8});
    CHECK_EQ(blk.t_out, uint32_t{120});
    CHECK_EQ(blk.source_in, uint32_t{2});
    CHECK_EQ(blk.speed, 2.0f);
    CHECK_EQ(d2.root().audio.size(), size_t{1});
    CHECK_EQ(d2.root().audio[0].name, "a1");
    CHECK_EQ(d2.root().audio[0].gain, 0.8f);
    CHECK_EQ(d2.root().audio[0].placements.size(), size_t{1});
    CHECK_EQ(d2.root().audio[0].placements[0].audio_gain, 0.5f);
    CHECK(d2.root().audio[0].placements[0].link != 0);
    CHECK_EQ(d2.root().audio[0].placements[0].link, blk.link);
    CHECK_EQ(d2.root().trim_in, uint32_t{4});
    CHECK_EQ(d2.root().trim_out, uint32_t{110});
    CHECK_EQ(d2.root().markers.size(), size_t{3});
    CHECK(d2.next_effect_id >= d.next_effect_id);
}

TEST(crt_parameters_and_cache_state) {
    Document d = doc_with_look();
    auto fx = make_effect(d, EffectType::CrtSim);
    fx.params.resize(9);
    fx.params[2] = 240.0f;
    fx.params[6] = 5.0f;
    d.looks[0].effects.push_back(fx);
    Document loaded = doc::doc_from_json(doc::doc_to_json(d));
    auto& restored = loaded.looks[0].effects[0];
    CHECK_EQ(restored.params.size(), size_t{13});
    CHECK_EQ(restored.params[2], 240.0f);
    CHECK_EQ(restored.params[6], 5.0f);
    CHECK_EQ(restored.params[9], 0.3f);
    CHECK_EQ(restored.params[11], 0.0f);
    CHECK(!doc::document_uses_history(loaded));
    restored.params[12] = 1.0f;
    CHECK(!doc::document_uses_history(loaded));
    restored.params[11] = 30.0f;
    CHECK(doc::document_uses_history(loaded));
    restored.params[4] = 3.0f;
    CHECK(!doc::document_uses_history(loaded));
    restored.params[4] = 0.0f;
    restored.bypass = true;
    CHECK(!doc::document_uses_history(loaded));
    restored.bypass = false;
    restored.params[11] = 0.0f;
    doc::KeyframeLane lane;
    lane.target = {restored.id, 11};
    lane.keys.push_back({0.0, 50.0f});
    loaded.looks[0].lanes.push_back(lane);
    CHECK(doc::document_uses_history(loaded));
    loaded.looks[0].lanes[0].muted = true;
    CHECK(!doc::document_uses_history(loaded));
}

TEST(serialize_tolerant_load) {
    // The loader skips unknown types and re-derives counters above the ids.
    const char* text = R"({
        "looks_project": 6,
        "name": "sparse",
        "next_effect_id": 1,
        "looks": [{"id": 100, "sources": [{"id": 40}], "effects": [
                {"type": "from_the_future", "id": 41},
                {"type": "vignette", "id": 42, "params": [0.9]}
            ]}]
    })";
    json::ParseResult parsed = json::parse(text);
    CHECK(parsed.value.has_value());
    Document d = doc::doc_from_json(*parsed.value);
    CHECK_EQ(d.looks[0].sources.size(), size_t{1});
    CHECK_EQ(d.looks[0].effects.size(), size_t{1});
    CHECK(d.looks[0].effects[0].type == EffectType::Vignette);
    CHECK_EQ(d.looks[0].effects[0].params[0], 0.9f);
    CHECK_EQ(d.looks[0].effects[0].params[1],
             doc::effect_info(EffectType::Vignette).params[1].default_value);
    CHECK(d.looks[0].sources[0].visible);
    CHECK_EQ(d.looks[0].sources[0].opacity, 1.0f);
    CHECK(d.next_effect_id > 42);   // not the stored 1

    CHECK(!json::parse("{nope").value.has_value());
}

TEST(serialize_v57_roundtrip) {
    Document d = doc_with_look();
    d.root().markers = {12, 45, 90};
    doc::KeyframeLane lane;
    lane.target = {d.looks[0].sources[0].id | doc::kSourceParamBit, 8};
    lane.keys.push_back({0.0, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    lane.keys.push_back({30.0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
    d.looks[0].lanes.push_back(lane);

    json::Value a = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(a);
    CHECK_EQ(d2.root().markers.size(), size_t{3});
    CHECK_EQ(d2.root().markers[1], uint32_t{45});
    CHECK_EQ(d2.looks[0].lanes.size(), size_t{1});
    CHECK_EQ(d2.looks[0].lanes[0].target.effect_id,
             d.looks[0].sources[0].id | doc::kSourceParamBit);
    CHECK_EQ(d2.looks[0].lanes[0].target.param_index, 8);
    json::Value b = doc::doc_to_json(d2);
    CHECK(a == b);
}

TEST(preset_insert_lands_dormant) {
    // Adding a preset never wires it: the members chain internally only.
    Document d = doc_with_look();
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    d.looks[0].links = {{d.looks[0].sources[0].id, d.looks[0].effects[0].id, 0},
        {d.looks[0].effects[0].id, 0, 0}};
    const auto original_links = d.looks[0].links;
    doc::UndoStack undo;

    doc::Group g;
    g.id = d.next_effect_id++;
    g.name = "preset";
    std::vector<doc::EffectInstance> members;
    members.push_back(make_effect(d, EffectType::Grain));
    members.push_back(make_effect(d, EffectType::Posterize));
    const uint64_t m0 = members[0].id, m1 = members[1].id;
    const uint64_t gid = g.id;
    g.inputs = {d.next_effect_id++};
    g.face_out = m1;
    for (auto& member : members) member.group_id = gid;
    undo.execute(d, doc::insert_group_command(d.looks[0].id, g, std::move(members),
        {{g.inputs[0], m0, 0}, {m0, m1, 0}}));

    // The group seeds its In slot, and nothing exterior touches the members.
    CHECK(!d.looks[0].links.empty());
    const doc::Group* placed = doc::find_group(d.looks[0], gid);
    CHECK(placed && placed->inputs.size() == size_t{1});
    const uint64_t slot0 = placed->inputs.front();
    bool internal = false, seeded = false, boundary = false,
         chain_out = false;
    for (const doc::NodeLink& l : d.looks[0].links) {
        if (l.from == m0 && l.to == m1 && l.to_port == 0) internal = true;
        if (l.from == slot0 && l.to == m0 && l.to_port == 0) seeded = true;
        if ((l.from == m1 || l.from == m0) && l.to == 0) boundary = true;
        if (l.to == m0 && l.from != slot0)
            boundary = true;   // nothing exterior feeds the group
        if (l.to == slot0) boundary = true;   // the slot sits unwired
        if (l.to == 0 && l.from == d.looks[0].effects[0].id)
            chain_out = true;
    }
    CHECK(internal);
    CHECK(seeded);
    CHECK(!boundary);
    CHECK(chain_out);

    undo.undo(d);
    CHECK(d.looks[0].links == original_links);
    CHECK_EQ(d.looks[0].groups.size(), size_t{0});
    CHECK_EQ(d.looks[0].effects.size(), size_t{1});
}

TEST(serialize_dedupes_input_fanin) {
    // Fan-in is legal on every image port.
    // Only an exact duplicate wire sheds, and the first one stays.
    const char* text = R"({
        "looks_project": 5,
        "root_look": 100,
        "looks": [{"id": 100,
        "layers": [{"id": 1, "stack": [
            {"type": "vignette", "id": 2},
            {"type": "grain", "id": 3}
        ]}],
        "links": [
            {"from": 1, "to": 2, "port": 0},
            {"from": 3, "to": 2, "port": 0},
            {"from": 1, "to": 2, "port": 0},
            {"from": 1, "to": 0, "port": 0},
            {"from": 2, "to": 0, "port": 0}
        ]}]
    })";
    json::ParseResult parsed = json::parse(text);
    CHECK(parsed.value.has_value());
    Document d = doc::doc_from_json(*parsed.value);
    int into_effect2 = 0, into_output = 0;
    uint64_t bottom = 0;
    bool first = true;
    for (const doc::NodeLink& l : d.looks[0].links) {
        if (l.to == 2 && l.to_port == 0) {
            ++into_effect2;
            if (first) bottom = l.from;
            first = false;
        }
        if (l.to == 0) ++into_output;
    }
    CHECK_EQ(into_effect2, 2);
    CHECK_EQ(bottom, uint64_t{1});
    CHECK_EQ(into_output, 2);
}

TEST(serialize_lane_keys_sorted_on_load) {
    // Eval assumes sorted keys, so the loader must sort them.
    const char* text = R"({
        "looks_project": 5,
        "root_look": 100,
        "looks": [{"id": 100,
        "layers": [{"id": 1, "stack": [{"type": "dither", "id": 2}]}],
        "lanes": [{"target": {"effect": 2, "param": 0},
                   "keys": [{"frame": 60, "value": 8},
                            {"frame": 0, "value": 2},
                            {"frame": 30, "value": 16}]}]}]
    })";
    json::ParseResult parsed = json::parse(text);
    CHECK(parsed.value.has_value());
    Document d = doc::doc_from_json(*parsed.value);
    CHECK_EQ(d.looks[0].lanes.size(), size_t{1});
    CHECK_EQ(d.looks[0].lanes[0].keys.size(), size_t{3});
    CHECK_EQ(d.looks[0].lanes[0].keys[0].frame, 0.0);
    CHECK_EQ(d.looks[0].lanes[0].keys[1].frame, 30.0);
    CHECK_EQ(d.looks[0].lanes[0].keys[2].frame, 60.0);
}

TEST(group_commands_lifecycle) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    d.looks[0].effects.push_back(make_effect(d, EffectType::Grain));
    d.looks[0].effects.push_back(make_effect(d, EffectType::Pixelate));

    doc::Group g = doc::make_group(d, "duo");
    undo.execute(d, doc::group_effects_command(d.looks[0].id, g, {d.looks[0].effects[0].id, d.looks[0].effects[1].id}));
    CHECK_EQ(d.looks[0].groups.size(), size_t{1});
    CHECK_EQ(d.looks[0].effects[0].group_id, g.id);
    CHECK_EQ(d.looks[0].effects[1].group_id, g.id);
    CHECK_EQ(d.looks[0].effects[2].group_id, uint64_t{0});

    const doc::ParamKey pk{d.looks[0].effects[1].id, 0};
    undo.execute(d, doc::set_group_exposed_command(d.looks[0].id, g.id, pk, true));
    CHECK_EQ(d.looks[0].groups[0].exposed.size(), size_t{1});
    undo.execute(d, doc::set_group_exposed_command(d.looks[0].id, g.id, pk, true));
    CHECK_EQ(d.looks[0].groups[0].exposed.size(), size_t{1});   // no dup
    undo.execute(d, doc::set_group_exposed_command(d.looks[0].id, g.id, pk, false));
    CHECK(d.looks[0].groups[0].exposed.empty());
    undo.undo(d);
    CHECK_EQ(d.looks[0].groups[0].exposed.size(), size_t{1});
    CHECK(d.looks[0].groups[0].exposed[0] == pk);

    undo.execute(d, doc::ungroup_command(d.looks[0].id, g.id));
    CHECK(d.looks[0].groups.empty());
    CHECK_EQ(d.looks[0].effects[0].group_id, uint64_t{0});
    undo.undo(d);
    CHECK_EQ(d.looks[0].groups.size(), size_t{1});
    CHECK_EQ(d.looks[0].groups[0].exposed.size(), size_t{1});
    CHECK_EQ(d.looks[0].effects[1].group_id, g.id);
}

TEST(group_bypass_compiles_out) {
    Document d = doc_with_look();
    doc::Asset media;
    media.id = d.next_effect_id++;
    media.frame_count = 100;
    d.assets.push_back(media);
    d.looks[0].sources[0].asset = media.id;
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    d.looks[0].effects.push_back(make_effect(d, EffectType::Pixelate));
    doc::Group g = doc::make_group(d, "off");
    g.bypass = true;
    d.looks[0].effects[0].group_id = g.id;
    d.looks[0].groups.push_back(g);

    d.looks[0].links.clear();
    connect_test_chain(d.looks[0], d.looks[0].sources[0].id,
        {d.looks[0].effects[0].id, d.looks[0].effects[1].id});
    gfx::RenderGraph graph = gfx::compile_graph(d, d.looks[0].id, 0);
    CHECK(graph.valid);
    // Source + pixelate only; the grouped vignette is compiled out.
    CHECK_EQ(graph.nodes.size(), size_t{2});
    CHECK_EQ(graph.nodes[1].effect_index, 1);
}

TEST(group_creation_slotifies_crossings) {
    // The exterior feed reroutes through a minted In slot.
    Document d = doc_with_look();
    doc::UndoStack undo;
    doc::Source& layer = d.looks[0].sources[0];
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    d.looks[0].effects.push_back(make_effect(d, EffectType::Grain));
    const uint64_t src = layer.id;
    const uint64_t f0 = d.looks[0].effects[0].id, f1 = d.looks[0].effects[1].id;
    d.looks[0].links = {{src, f0, 0}, {f0, f1, 0}, {f1, 0, 0}};
    const std::vector<doc::NodeLink> before = d.looks[0].links;

    doc::Group g = doc::make_group(d, "wrap");
    const uint64_t gid = g.id;
    undo.execute(d, doc::group_effects_command(d.looks[0].id, g, {d.looks[0].effects[0].id, d.looks[0].effects[1].id}));
    const doc::Group* placed = doc::find_group(d.looks[0], gid);
    CHECK(placed && placed->inputs.size() == size_t{1});
    const uint64_t slot = placed->inputs.front();
    bool exterior = false, interior = false, raw_crossing = false;
    for (const doc::NodeLink& l : d.looks[0].links) {
        if (l.from == src && l.to == slot && l.to_port == 0)
            exterior = true;
        if (l.from == slot && l.to == f0 && l.to_port == 0)
            interior = true;
        if (l.from == src && l.to == f0) raw_crossing = true;
    }
    CHECK(exterior);
    CHECK(interior);
    CHECK(!raw_crossing);
    // The chain still compiles to source -> vignette -> grain.
    (void)f1;
    doc::Asset media;
    media.id = d.next_effect_id++;
    media.frame_count = 100;
    d.assets.push_back(media);
    d.looks[0].sources[0].asset = media.id;
    gfx::RenderGraph graph = gfx::compile_graph(d, d.looks[0].id, 0);
    CHECK(graph.valid);
    CHECK_EQ(graph.nodes.size(), size_t{3});

    undo.undo(d);
    CHECK_EQ(d.looks[0].links.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK_EQ(d.looks[0].links[i].from, before[i].from);
        CHECK_EQ(d.looks[0].links[i].to, before[i].to);
        CHECK_EQ(d.looks[0].links[i].to_port, before[i].to_port);
    }
    CHECK(d.looks[0].groups.empty());
    undo.redo(d);
    const doc::Group* again = doc::find_group(d.looks[0], gid);
    CHECK(again && again->inputs.size() == size_t{1});
    // Redo replays the SAME slot id - undo/redo never grows the space.
    CHECK_EQ(again->inputs.front(), slot);
}

TEST(group_wet_serializes_and_snapshots) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    d.looks[0].effects.push_back(make_effect(d, EffectType::Grain));
    doc::Group g = doc::make_group(d, "wetter");
    g.wet = 0.25f;
    g.opacity = 0.5f;
    const uint64_t gid = g.id;
    d.looks[0].effects[0].group_id = gid;
    d.looks[0].groups.push_back(g);

    json::Value out = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(out);
    const doc::Group* g2 = doc::find_group(d2.looks[0], gid);
    CHECK(g2);
    CHECK(std::fabs(g2->wet - 0.25f) < 1e-6f);
    CHECK(std::fabs(g2->opacity - 0.5f) < 1e-6f);

    undo.execute(d, doc::store_snapshot_command(d.looks[0].id, 0));
    doc::find_group(d.looks[0], gid)->wet = 1.0f;
    undo.execute(d, doc::apply_snapshot_command(d.looks[0].id, 0));
    CHECK(std::fabs(doc::find_group(d.looks[0], gid)->wet - 0.25f) <
          1e-6f);
    // Snapshot entries survive serialization with the id bit split out.
    json::Value snap = doc::doc_to_json(d);
    Document d3 = doc::doc_from_json(snap);
    bool found = false;
    for (const doc::SnapshotEntry& e : d3.looks[0].snapshots[0].entries)
        if (e.effect_id == (gid | doc::kGroupParamBit)) {
            found = true;
            CHECK(std::fabs(e.wet - 0.25f) < 1e-6f);
        }
    CHECK(found);
}

TEST(group_wet_compiles_the_mix_wrapper) {
    Document d = doc_with_look();
    doc::Asset media;
    media.id = d.next_effect_id++;
    media.frame_count = 100;
    d.assets.push_back(media);
    doc::Source& layer = d.looks[0].sources[0];
    layer.asset = media.id;
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    doc::UndoStack undo;
    doc::Group g = doc::make_group(d, "mixed");
    const uint64_t gid = g.id;
    d.looks[0].links = {{layer.id, d.looks[0].effects[0].id, 0},
        {d.looks[0].effects[0].id, 0, 0}};
    undo.execute(d, doc::group_effects_command(d.looks[0].id, g, {d.looks[0].effects[0].id}));

    // Identity knobs, no matte: no wrapper node.
    gfx::RenderGraph plain = gfx::compile_graph(d, d.looks[0].id, 0);
    CHECK(plain.valid);
    for (const gfx::GraphNode& n : plain.nodes)
        CHECK(n.kind != gfx::GraphNode::Kind::GroupMix);

    // With wet below 1 the face re-lands as GroupMix{dry, face}.
    doc::find_group(d.looks[0], gid)->wet = 0.5f;
    gfx::RenderGraph mixed = gfx::compile_graph(d, d.looks[0].id, 0);
    CHECK(mixed.valid);
    int mix_at = -1, fx_at = -1, src_at = -1;
    for (size_t i = 0; i < mixed.nodes.size(); ++i) {
        if (mixed.nodes[i].kind == gfx::GraphNode::Kind::GroupMix)
            mix_at = static_cast<int>(i);
        if (mixed.nodes[i].kind == gfx::GraphNode::Kind::Effect)
            fx_at = static_cast<int>(i);
        if (mixed.nodes[i].kind == gfx::GraphNode::Kind::Source)
            src_at = static_cast<int>(i);
    }
    CHECK(mix_at >= 0 && fx_at >= 0 && src_at >= 0);
    CHECK_EQ(mixed.nodes[static_cast<size_t>(mix_at)].inputs.size(),
             size_t{2});
    CHECK_EQ(mixed.nodes[static_cast<size_t>(mix_at)].inputs[0], src_at);
    CHECK_EQ(mixed.nodes[static_cast<size_t>(mix_at)].inputs[1], fx_at);
    CHECK_EQ(mixed.nodes[static_cast<size_t>(mix_at)].effect_index, 0);
    CHECK_EQ(mixed.output, mix_at);

    // A port-1 wire on a group id gates it like any effect.
    doc::Source matte_layer;
    matte_layer.id = d.next_effect_id++;
    matte_layer.source = doc::SourceKind::Shape;
    d.looks[0].sources.push_back(matte_layer);
    d.looks[0].links.push_back({matte_layer.id, gid, 1});
    gfx::RenderGraph gated = gfx::compile_graph(d, d.looks[0].id, 0);
    CHECK(gated.valid);
    bool extract = false, apply = false;
    for (const gfx::GraphNode& n : gated.nodes) {
        if (n.kind == gfx::GraphNode::Kind::MatteExtract) extract = true;
        if (n.kind == gfx::GraphNode::Kind::MatteApply) apply = true;
    }
    CHECK(extract);
    CHECK(apply);
}

TEST(ungroup_splices_slots_back_to_direct_links) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    doc::Source& layer = d.looks[0].sources[0];
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));
    const uint64_t src = layer.id;
    const uint64_t f0 = d.looks[0].effects[0].id;
    d.looks[0].links = {{src, f0, 0}, {f0, 0, 0}};
    doc::Group g = doc::make_group(d, "temp");
    const uint64_t gid = g.id;
    undo.execute(d, doc::group_effects_command(d.looks[0].id, g, {d.looks[0].effects[0].id}));
    const uint64_t slot =
        doc::find_group(d.looks[0], gid)->inputs.front();

    undo.execute(d, doc::ungroup_command(d.looks[0].id, gid));
    bool direct = false, slotted = false;
    for (const doc::NodeLink& l : d.looks[0].links) {
        if (l.from == src && l.to == f0 && l.to_port == 0) direct = true;
        if (l.from == slot || l.to == slot) slotted = true;
    }
    CHECK(direct);
    CHECK(!slotted);
    undo.undo(d);
    const doc::Group* back = doc::find_group(d.looks[0], gid);
    CHECK(back && back->inputs.size() == size_t{1} &&
          back->inputs.front() == slot);
}

TEST(grouping_preserves_interleaved_feeds_and_exact_undo) {
    Document d = doc_with_look();
    auto& look = d.looks[0];
    look.sources[0].source = doc::SourceKind::Solid;
    look.sources.push_back(doc::make_source(d, doc::SourceKind::Noise));
    look.effects.push_back(make_effect(d, EffectType::Invert));
    look.effects.push_back(make_effect(d, EffectType::Blur));
    const uint64_t a = look.sources[0].id, b = look.sources[1].id;
    const uint64_t x = look.effects[0].id, y = look.effects[1].id;
    look.links = {{a, x, 0}, {a, y, 0}, {x, y, 0}, {b, y, 0}, {y, 0, 0}};
    const auto original = look.links;
    auto group = doc::make_group(d, "interleaved");
    group.face_out = y;
    doc::UndoStack undo;
    undo.execute(d, doc::group_effects_command(look.id, group, {y, x}));
    CHECK_EQ(look.groups[0].inputs.size(), size_t{3});
    const auto grouped = look.links;
    std::vector<uint64_t> feeds;
    for (const auto& link : look.links) {
        if (link.to != y || link.to_port) continue;
        uint64_t producer = link.from;
        if (doc::group_of_input(look, producer))
            for (const auto& input : look.links)
                if (input.to == producer && !input.to_port) { producer = input.from; break; }
        feeds.push_back(producer);
    }
    CHECK(feeds == std::vector<uint64_t>({a, x, b}));
    undo.execute(d, doc::ungroup_command(look.id, group.id));
    CHECK(look.links == original);
    undo.undo(d);
    CHECK(look.links == grouped);
    undo.undo(d);
    CHECK(look.links == original);
    CHECK(look.groups.empty());
    undo.redo(d);
    CHECK(look.links == grouped);
}

TEST(preset_preserves_branches_auxiliary_ports_and_input_ids) {
    Document d = doc_with_look();
    auto& look = d.looks[0];
    look.effects.push_back(make_effect(d, EffectType::Invert));
    look.effects.push_back(make_effect(d, EffectType::Blur));
    look.effects.push_back(make_effect(d, EffectType::BlendNode));
    auto group = doc::make_group(d, "branch");
    group.inputs = {d.next_effect_id++, d.next_effect_id++};
    const uint64_t a = look.effects[0].id, b = look.effects[1].id, c = look.effects[2].id;
    group.face_out = c;
    for (auto& fx : look.effects) fx.group_id = group.id;
    look.groups.push_back(group);
    look.links = {{look.sources[0].id, group.inputs[0], 0},
        {group.inputs[0], a, 0}, {group.inputs[0], b, 0},
        {a, c, 0}, {b, c, 0}, {group.inputs[1], c, 2}, {c, 0, 0}};
    const auto preset = doc::make_preset_from_group(look, group.id);
    CHECK_EQ(preset.links.size(), size_t{5});
    const auto parsed = doc::preset_from_json(doc::preset_to_json(preset));
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(parsed->links == preset.links);
    auto instance = doc::instantiate_preset(d, *parsed);
    const auto& links = instance.links;
    CHECK_EQ(instance.group.inputs.size(), size_t{2});
    CHECK(instance.group.inputs[0] != group.inputs[0]);
    CHECK_EQ(links[0].from, instance.group.inputs[0]);
    CHECK_EQ(links[1].from, instance.group.inputs[0]);
    CHECK_EQ(links[2].from, instance.effects[0].id);
    CHECK_EQ(links[3].from, instance.effects[1].id);
    CHECK_EQ(links[4].from, instance.group.inputs[1]);
    CHECK_EQ(links[4].to_port, uint32_t{2});
    CHECK_EQ(instance.group.face_out, instance.effects[2].id);
    auto bad = doc::preset_to_json(preset);
    bad.set("looks_preset", 1);
    CHECK(!doc::preset_from_json(bad));
}

TEST(preset_capture_and_instantiate) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    d.looks[0].effects.push_back(make_effect(d, EffectType::FilmStock));
    d.looks[0].effects.push_back(make_effect(d, EffectType::Grain));
    d.looks[0].effects[0].params[3] = 0.3f;

    doc::Group g = doc::make_group(d, "era");
    g.exposed.push_back({d.looks[0].effects[0].id, 3});
    // A dangling face key (no such member) must be dropped on capture.
    g.exposed.push_back({9999, 0});
    d.looks[0].effects[0].group_id = g.id;
    d.looks[0].effects[1].group_id = g.id;
    g.inputs = {d.next_effect_id++};
    g.face_out = d.looks[0].effects[1].id;
    d.looks[0].links = {{d.looks[0].sources[0].id, g.inputs[0], 0},
        {g.inputs[0], d.looks[0].effects[0].id, 0},
        {d.looks[0].effects[0].id, d.looks[0].effects[1].id, 0},
        {d.looks[0].effects[1].id, 0, 0}};
    d.looks[0].groups.push_back(g);

    doc::Preset p = doc::make_preset_from_group(d.looks[0], g.id);
    CHECK_EQ(p.name, "era");
    CHECK_EQ(p.effects.size(), size_t{2});
    CHECK_EQ(p.group.exposed.size(), size_t{1});

    json::Value pj = doc::preset_to_json(p);
    auto p2 = doc::preset_from_json(pj);
    CHECK(p2.has_value());
    CHECK(doc::preset_to_json(*p2) == pj);

    Document target = doc_with_look();
    auto instance = doc::instantiate_preset(target, *p2);
    auto& ng = instance.group;
    auto& nfx = instance.effects;
    CHECK_EQ(nfx.size(), size_t{2});
    CHECK(nfx[0].id != p2->effects[0].id);
    CHECK_EQ(nfx[0].group_id, ng.id);
    CHECK_EQ(ng.exposed.size(), size_t{1});
    CHECK_EQ(ng.exposed[0].effect_id, nfx[0].id);
    CHECK_EQ(instance.links[0].from, ng.inputs[0]);
    CHECK_EQ(instance.links[0].to, nfx[0].id);
    CHECK(ng.folded);

    undo.execute(target, doc::insert_group_command(target.looks[0].id, ng, nfx, instance.links));
    CHECK_EQ(target.looks[0].effects.size(), size_t{2});
    CHECK_EQ(target.looks[0].groups.size(), size_t{1});
    undo.undo(target);
    CHECK(target.looks[0].effects.empty());
    CHECK(target.looks[0].groups.empty());
    undo.redo(target);
    CHECK_EQ(target.looks[0].effects.size(), size_t{2});
}

TEST(morph_interpolates_snapshots) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    d.looks[0].effects.push_back(make_effect(d, EffectType::Vignette));

    d.looks[0].effects[0].params[0] = 0.0f;
    undo.execute(d, doc::store_snapshot_command(d.looks[0].id,0));
    d.looks[0].effects[0].params[0] = 1.0f;
    undo.execute(d, doc::store_snapshot_command(d.looks[0].id,1));
    d.looks[0].effects[0].params[0] = 0.5f;   // overridden while morphing

    undo.execute(d, doc::set_morph_command(d.looks[0].id,0, 1, 0.25f));
    Document r = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(std::fabs(r.looks[0].effects[0].params[0] - 0.25f) < 1e-5f);

    // The morph position is the mod target {0, 0}; a sine at t=0 reads 0.5.
    doc::ValueNode sine;
    sine.id = d.next_route_id++;
    d.looks[0].value_nodes.push_back(sine);
    doc::ModRoute route;
    route.id = d.next_route_id++;
    route.node = sine.id;
    route.target = {0, 0};
    d.looks[0].mod_routes.push_back(route);
    r = mod::resolve(d, 0, 30.0, nullptr);
    CHECK(std::fabs(r.looks[0].effects[0].params[0] - 0.5f) < 1e-4f);

    undo.undo(d);
    CHECK_EQ(d.looks[0].morph_pos, 0.0f);
    undo.redo(d);
    json::Value j = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(j);
    CHECK_EQ(d2.looks[0].morph_pos, 0.25f);
    CHECK_EQ(d2.looks[0].morph_to, 1);
    CHECK(doc::doc_to_json(d2) == j);
}

TEST(era_presets_ship_valid) {
    // Every exposed face param must resolve to a member effect.
    const std::filesystem::path dir =
        std::filesystem::path(LOOKS_REPO_ROOT) / "assets" / "presets";
    std::vector<doc::Preset> presets = doc::scan_presets(dir);
    CHECK_EQ(presets.size(), size_t{24});
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

TEST(serialize_lane_flags_roundtrip) {
    // The hidden flag changes what exports, so it is document state.
    Document d = doc_with_look();
    d.root().tracks[0].hidden = true;
    d.root().tracks[0].lock = true;
    d.root().audio[0].lock = true;
    json::Value v = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(v);
    CHECK(d2.root().tracks[0].hidden);
    CHECK(d2.root().tracks[0].lock);
    CHECK(d2.root().audio[0].lock);
    CHECK(doc::doc_to_json(d2) == v);

    Document plain;
    Document plain2 = doc::doc_from_json(doc::doc_to_json(plain));
    CHECK(!plain2.root().tracks[0].hidden);
    CHECK(!plain2.root().tracks[0].lock);
    CHECK(!plain2.root().audio[0].lock);
}

TEST(serialize_gradient_stops_roundtrip) {
    Document d = doc_with_look();
    doc::Source& l = d.looks[0].sources[0];
    l.source = doc::SourceKind::Gradient;
    l.color_a[3] = 0.25f;
    l.color_b[3] = 0.5f;
    doc::GradientStop a;
    a.id = d.next_effect_id++;
    a.t = 0.25f;
    a.x = 0.2f;
    a.y = 0.8f;
    a.color[0] = 1.0f;
    a.color[3] = 0.0f;
    doc::GradientStop b;
    b.id = d.next_effect_id++;
    b.t = 0.75f;
    b.x = 0.6f;
    b.y = 0.4f;
    b.color[2] = 1.0f;
    b.color[3] = 0.5f;
    l.stops.push_back(a);
    l.stops.push_back(b);

    json::Value v = doc::doc_to_json(d);
    Document d2 = doc::doc_from_json(v);
    const doc::Source& g = d2.looks[0].sources[0];
    CHECK_EQ(g.stops.size(), size_t{2});
    CHECK_EQ(g.stops[0].t, 0.25f);
    CHECK_EQ(g.stops[1].t, 0.75f);
    CHECK_EQ(g.stops[0].x, 0.2f);
    CHECK_EQ(g.stops[1].y, 0.4f);
    CHECK_EQ(g.stops[0].color[3], 0.0f);
    CHECK_EQ(g.stops[1].color[3], 0.5f);
    CHECK_EQ(g.color_a[3], 0.25f);
    CHECK_EQ(g.color_b[3], 0.5f);
    CHECK(doc::doc_to_json(d2) == v);
}
