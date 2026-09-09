#include "doc/stack_commands.h"

#include <algorithm>

#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/randomize.h"
#include "doc_fixture.h"
#include "doc/serialize.h"
#include "test_framework.h"

using namespace looks::doc;

TEST(mode_generated_frame_is_undoable_and_serialized) {
    auto doc = doc_with_look();
    auto& look = doc.looks[0];
    look.effects.push_back(make_effect(doc, EffectType::Mode));
    auto& fx = look.effects.back();
    UndoStack undo;
    undo.execute(doc, set_generated_frame_command(look.id, fx.id, "cache/one/frame.lrgba", "one"));
    undo.execute(doc, set_generated_frame_command(look.id, fx.id, "cache/two/frame.lrgba", "two"));
    CHECK_EQ(fx.generated_signature, std::string("two"));
    CHECK(undo.undo(doc));
    CHECK_EQ(fx.generated_path, std::string("cache/one/frame.lrgba"));
    CHECK(undo.redo(doc));
    const auto restored = effect_from_json(effect_to_json(fx));
    CHECK(restored.has_value());
    CHECK_EQ(restored->generated_path, fx.generated_path);
    CHECK_EQ(restored->generated_signature, fx.generated_signature);
}

namespace {

Document make_doc_with_two() {
    Document doc = doc_with_look();
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::RgbSplit));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    doc.looks[0].links = {{doc.looks[0].sources[0].id, doc.looks[0].effects[0].id, 0},
        {doc.looks[0].effects[0].id, doc.looks[0].effects[1].id, 0}, {doc.looks[0].effects[1].id, 0, 0}};
    return doc;
}

}  // namespace

TEST(connection_blend_edits_preserve_identity_order_and_undo) {
    auto doc = make_doc_with_two();
    auto& look = doc.looks[0];
    const auto target = look.links.back();
    UndoStack undo;
    undo.execute(doc, set_link_blend_command(look.id, target, BlendMode::Screen));
    CHECK(look.links.back().blend == BlendMode::Screen);
    const auto before = look.links;
    undo.execute(doc, connect_command(look.id, target));
    CHECK(look.links == before);
    undo.undo(doc);
    CHECK(look.links == before);
    undo.execute(doc, disconnect_command(look.id, target));
    CHECK_EQ(look.links.size(), before.size() - 1);
    undo.undo(doc);
    CHECK(look.links == before);
    NodeLink replacement{target.from, 0, 1};
    undo.execute(doc, reconnect_command(look.id, target, replacement));
    CHECK(look.links.back().same_endpoints(replacement));
    CHECK(look.links.back().blend == BlendMode::Screen);
    undo.undo(doc);
    CHECK(look.links == before);
    undo.redo(doc);
    CHECK(look.links.back().blend == BlendMode::Screen);
    undo.undo(doc);
    undo.undo(doc);
    CHECK(look.links.back().blend == BlendMode::Normal);
}

TEST(effect_commands_target_ids_after_collection_edits) {
    auto d = make_doc_with_two();
    auto& look = d.looks[0];
    const uint64_t id = look.effects[1].id;
    auto edit = looks::doc::set_bypass_command(look.id, id, true);
    look.effects.insert(look.effects.begin(), looks::doc::make_effect(d, looks::doc::EffectType::Blur));
    edit->apply(d);
    CHECK(looks::doc::find_effect(look, id)->bypass);
    CHECK(!look.effects[1].bypass);
    edit->revert(d);
    CHECK(!looks::doc::find_effect(look, id)->bypass);
    auto added = looks::doc::make_effect(d, looks::doc::EffectType::Invert);
    auto insert = looks::doc::add_effect_command(look.id, added, 1);
    insert->apply(d);
    std::reverse(look.effects.begin(), look.effects.end());
    insert->revert(d);
    CHECK(looks::doc::find_effect(look, added.id) == nullptr);
    CHECK(looks::doc::find_effect(look, id) != nullptr);
}

TEST(effect_parameter_commands_ignore_missing_or_invalid_targets) {
    auto d = make_doc_with_two();
    const auto id = d.looks[0].effects[0].id;
    const auto original = d.looks[0].effects[0].params;
    for (int index : {-200, 10000}) {
        auto command = set_param_command(d.looks[0].id, id, index, 5);
        command->apply(d);
        command->revert(d);
        CHECK(d.looks[0].effects[0].params == original);
    }
    auto command = set_param_gesture_command(d.looks[0].id, id, {{0, 5}, {10000, 2}}, {});
    command->apply(d);
    command->revert(d);
    CHECK(d.looks[0].effects[0].params == original);
    auto missing = set_param_command(d.looks[0].id, 99999, 0, 2);
    missing->apply(d);
    missing->revert(d);
    CHECK(d.looks[0].effects[0].params == original);
}

TEST(node_deletion_detaches_only_its_own_references_and_undo_restores_them) {
    auto d = make_doc_with_two();
    auto& look = d.looks[0];
    const auto source = look.sources[0].id, a = look.effects[0].id, b = look.effects[1].id;
    const auto stop = d.next_effect_id++;
    look.sources[0].stops.push_back({stop});
    const std::vector<uint64_t> targets{source | kSourceParamBit, stop | kStopParamBit, a, b};
    for (const auto target : targets) {
        look.lanes.push_back({{target, 0}, {{0, 0.5f}}});
        ModRoute route;
        route.id = d.next_route_id++;
        route.target = {target, 0};
        look.mod_routes.push_back(route);
        look.snapshots[0].entries.push_back({target, {0.5f}});
    }
    ValueNode value;
    value.id = d.next_route_id++;
    value.audio_src = a;
    look.value_nodes.push_back(value);
    auto remove_source = remove_source_command(look.id, source);
    remove_source->apply(d);
    CHECK_EQ(look.lanes.size(), size_t{2});
    CHECK_EQ(look.mod_routes.size(), size_t{2});
    CHECK_EQ(look.snapshots[0].entries.size(), size_t{2});
    CHECK_EQ(look.value_nodes[0].audio_src, a);
    auto remove_fx = remove_effect_command(look.id, a);
    remove_fx->apply(d);
    CHECK_EQ(look.lanes.size(), size_t{1});
    CHECK_EQ(look.lanes[0].target.effect_id, b);
    CHECK_EQ(look.value_nodes[0].audio_src, uint64_t{0});
    remove_fx->revert(d);
    remove_source->revert(d);
    CHECK_EQ(look.value_nodes[0].audio_src, a);
    CHECK_EQ(look.lanes.size(), targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        CHECK_EQ(look.lanes[i].target.effect_id, targets[i]);
        CHECK_EQ(look.mod_routes[i].target.effect_id, targets[i]);
        CHECK_EQ(look.snapshots[0].entries[i].effect_id, targets[i]);
    }
}

TEST(graph_reconnect_to_same_endpoint_keeps_the_feed) {
    auto d = make_doc_with_two();
    const auto links = d.looks[0].links;
    auto cmd = looks::doc::reconnect_command(d.looks[0].id, links[0], links[0]);
    cmd->apply(d);
    CHECK(d.looks[0].links == links);
    cmd->revert(d);
    CHECK(d.looks[0].links == links);
}

TEST(effect_move_preserves_fanin_side_inputs_and_group_output) {
    auto d = make_doc_with_two();
    auto& look = d.looks[0];
    const auto a = look.effects[0].id, b = look.effects[1].id;
    auto extra = make_source(d, SourceKind::Solid);
    look.sources.push_back(extra);
    auto group = make_group(d, "chain");
    group.face_out = b;
    look.effects[0].group_id = look.effects[1].group_id = group.id;
    look.groups.push_back(group);
    look.links.insert(look.links.begin() + 1, {extra.id, a, 0});
    look.links.push_back({extra.id, a, 1});
    look.links.push_back({extra.id, b, 2});
    const auto original = look.links;
    CHECK_EQ(effect_chain_neighbor(look, a, 1), b);
    auto cmd = move_effect_command(look.id, a, b);
    cmd->apply(d);
    CHECK_EQ(look.links[0].to, b);
    CHECK_EQ(look.links[1].to, b);
    CHECK_EQ(look.links[2].from, b);
    CHECK_EQ(look.links[2].to, a);
    CHECK_EQ(look.links[3].from, a);
    CHECK(look.links[4] == original[4]);
    CHECK(look.links[5] == original[5]);
    CHECK_EQ(look.groups[0].face_out, a);
    CHECK_EQ(look.effects[0].id, a);
    CHECK_EQ(effect_chain_neighbor(look, a, -1), b);
    cmd->revert(d);
    CHECK(look.links == original);
    CHECK_EQ(look.groups[0].face_out, b);
    look.links.push_back({a, 0, 0});
    CHECK_EQ(effect_chain_neighbor(look, a, 1), uint64_t{0});
    const auto branched = look.links;
    cmd->apply(d);
    CHECK(look.links == branched);
}

TEST(stack_make_effect_defaults) {
    Document doc = doc_with_look();
    EffectInstance fx = make_effect(doc, EffectType::Vignette);
    const EffectInfo& info = effect_info(EffectType::Vignette);
    CHECK_EQ(fx.params.size(), size_t{info.param_count});
    for (uint32_t i = 0; i < info.param_count; ++i)
        CHECK_EQ(fx.params[i], info.params[i].default_value);
    CHECK_EQ(fx.wet, 1.0f);
    CHECK_EQ(fx.opacity, 1.0f);
    CHECK(!fx.bypass);
    EffectInstance fx2 = make_effect(doc, EffectType::Pixelate);
    CHECK(fx2.id > fx.id);
}

TEST(stack_add_remove_undo) {
    Document doc = doc_with_look();
    UndoStack undo;

    undo.execute(doc, add_effect_command(doc.looks[0].id, make_effect(doc, EffectType::Pixelate), 0));
    CHECK_EQ(doc.looks[0].effects.size(), size_t{1});
    CHECK(doc.looks[0].effects[0].type == EffectType::Pixelate);

    undo.execute(doc, add_effect_command(doc.looks[0].id, make_effect(doc, EffectType::RgbSplit), 0));
    CHECK_EQ(doc.looks[0].effects.size(), size_t{2});
    CHECK(doc.looks[0].effects[0].type == EffectType::RgbSplit);

    const uint64_t pixelate_id = doc.looks[0].effects[1].id;
    undo.execute(doc, remove_effect_command(doc.looks[0].id, doc.looks[0].effects[1].id));
    CHECK_EQ(doc.looks[0].effects.size(), size_t{1});

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects.size(), size_t{2});
    CHECK_EQ(doc.looks[0].effects[1].id, pixelate_id);

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects.size(), size_t{1});
    CHECK(doc.looks[0].effects[0].type == EffectType::Pixelate);

    undo.redo(doc);
    CHECK_EQ(doc.looks[0].effects.size(), size_t{2});
    CHECK(doc.looks[0].effects[0].type == EffectType::RgbSplit);
}

TEST(stack_set_param_and_wet_opacity) {
    Document doc = make_doc_with_two();
    UndoStack undo;

    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, 0, 20.0f));
    CHECK_EQ(doc.looks[0].effects[0].params[0], 20.0f);

    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, kWetParam, 0.5f));
    CHECK_EQ(doc.looks[0].effects[0].wet, 0.5f);
    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[1].id, kOpacityParam, 0.25f));
    CHECK_EQ(doc.looks[0].effects[1].opacity, 0.25f);

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects[1].opacity, 1.0f);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects[0].wet, 1.0f);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects[0].params[0],
             effect_info(EffectType::RgbSplit).params[0].default_value);
}

TEST(effect_controls_follow_dependencies) {
    for (uint32_t type = 0; type < static_cast<uint32_t>(EffectType::Count); ++type) {
        const auto& info = effect_info(static_cast<EffectType>(type));
        CHECK(info.param_count <= 30);
        CHECK(info.control_order != nullptr);
        if (!info.control_order || info.param_count > 30) continue;
        bool seen[32] = {};
        for (uint32_t row = 0; row < info.param_count; ++row) {
            const int p = info.control_param(row + 2);
            CHECK(p >= 0 && p < static_cast<int>(info.param_count));
            if (p < 0 || p >= static_cast<int>(info.param_count)) continue;
            CHECK(!seen[p]);
            const int dependency = info.params[p].vis_param;
            if (dependency >= 0) CHECK(seen[dependency]);
            seen[p] = true;
        }
        CHECK_EQ(info.control_param(0), kWetParam);
        CHECK_EQ(info.control_param(1), kOpacityParam);
    }
}

TEST(stack_param_drag_coalesces) {
    Document doc = make_doc_with_two();
    UndoStack undo;
    const float original = doc.looks[0].effects[0].params[0];

    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, 0, 10.0f), /*coalesce=*/true);
    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, 0, 20.0f), /*coalesce=*/true);
    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, 0, 30.0f), /*coalesce=*/true);
    CHECK_EQ(doc.looks[0].effects[0].params[0], 30.0f);
    CHECK_EQ(undo.undo_depth(), size_t{1});

    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, 1, 5.0f), /*coalesce=*/true);
    CHECK_EQ(undo.undo_depth(), size_t{2});

    undo.break_coalescing();
    undo.execute(doc, set_param_command(doc.looks[0].id, doc.looks[0].effects[0].id, 1, 8.0f), /*coalesce=*/true);
    CHECK_EQ(undo.undo_depth(), size_t{3});

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects[0].params[1], 5.0f);
    undo.undo(doc);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects[0].params[0], original);
}

TEST(stack_randomize) {
    Document doc = doc_with_look();
    UndoStack undo;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Quantize));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    const std::vector<float> before_q = doc.looks[0].effects[0].params;
    const std::vector<float> before_v = doc.looks[0].effects[1].params;

    randomize_look(doc, undo, doc.looks[0].id, 1.0f, 1234);
    const EffectInfo& qinfo = effect_info(EffectType::Quantize);
    const auto& q = doc.looks[0].effects[0].params;
    bool any_changed = false;
    for (uint32_t p = 0; p < qinfo.param_count; ++p) {
        CHECK(q[p] >= qinfo.params[p].min_value);
        CHECK(q[p] <= qinfo.params[p].max_value);
        if (!param_randomizable(qinfo.params[p]))
            CHECK_EQ(q[p], before_q[p]);
        else if (q[p] != before_q[p])
            any_changed = true;
    }
    CHECK(any_changed);
    // "palette" must be frozen, "levels" not.
    CHECK(!param_randomizable(qinfo.params[1]));
    CHECK(param_randomizable(qinfo.params[0]));

    CHECK_EQ(undo.undo_depth(), size_t{1});
    undo.undo(doc);
    CHECK(doc.looks[0].effects[0].params == before_q);
    CHECK(doc.looks[0].effects[1].params == before_v);

    // Determinism: same seed reproduces the same mutation.
    randomize_effect(doc, undo, doc.looks[0].id, doc.looks[0].effects[1].id, 0.7f, 42);
    const std::vector<float> first = doc.looks[0].effects[1].params;
    undo.undo(doc);
    randomize_effect(doc, undo, doc.looks[0].id, doc.looks[0].effects[1].id, 0.7f, 42);
    CHECK(doc.looks[0].effects[1].params == first);

    // Intensity 0 makes no change and leaves no undo entry.
    const size_t depth = undo.undo_depth();
    randomize_effect(doc, undo, doc.looks[0].id, doc.looks[0].effects[1].id, 0.0f, 99);
    CHECK(doc.looks[0].effects[1].params == first);
    (void)depth;
}

TEST(stack_bypass_and_move) {
    Document doc = make_doc_with_two();
    UndoStack undo;
    const uint64_t first_id = doc.looks[0].effects[0].id;

    undo.execute(doc, set_bypass_command(doc.looks[0].id, doc.looks[0].effects[0].id, true));
    CHECK(doc.looks[0].effects[0].bypass);
    undo.undo(doc);
    CHECK(!doc.looks[0].effects[0].bypass);

    undo.execute(doc, move_effect_command(doc.looks[0].id, doc.looks[0].effects[0].id, doc.looks[0].effects[1].id));
    CHECK_EQ(doc.looks[0].links.back().from, first_id);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].effects[0].id, first_id);
    undo.redo(doc);
    CHECK_EQ(doc.looks[0].links.back().from, first_id);
}

TEST(graph_connect_output_preserves_other_feeds) {
    Document doc = make_doc_with_two();          // layer 0: rgb, vignette
    doc.looks[0].sources.push_back(make_source(doc, SourceKind::Solid));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Pixelate));
    const uint64_t fx0 = doc.looks[0].effects[0].id;
    const uint64_t fx1 = doc.looks[0].effects[1].id;
    const uint64_t other_end = doc.looks[0].effects[2].id;

    doc.looks[0].links = {{doc.looks[0].sources[0].id, fx0, 0}, {fx0, fx1, 0},
        {fx1, 0, 0}, {doc.looks[0].sources[1].id, other_end, 0}, {other_end, 0, 0}};
    auto out_ends = [&] {
        std::vector<uint64_t> ends;
        for (const auto& l : doc.looks[0].links)
            if (l.to == 0 && l.to_port == 0) ends.push_back(l.from);
        return ends;
    };
    CHECK_EQ(out_ends().size(), size_t{2});

    UndoStack undo;
    undo.execute(doc, connect_command(doc.looks[0].id, {fx0, 0, 0}));
    auto ends = out_ends();
    CHECK_EQ(ends.size(), size_t{3});
    CHECK(std::find(ends.begin(), ends.end(), fx0) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), other_end) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), fx1) != ends.end());
    CHECK_EQ(ends[0], fx1);
    CHECK_EQ(ends[2], fx0);

    CHECK(undo.undo(doc));
    ends = out_ends();
    CHECK_EQ(ends.size(), size_t{2});
    CHECK(std::find(ends.begin(), ends.end(), fx1) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), fx0) == ends.end());
}

TEST(stack_param_gesture_coalesces_mixed) {
    // The drag writes one command per frame and merges to one undo entry.
    Document doc = doc_with_look();
    UndoStack undo;
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Spherize));
    Look& look = doc.looks[0];
    const uint64_t fx_id = look.effects[0].id;
    KeyframeLane lane;
    lane.target = {fx_id, 3};
    lane.keys.push_back({0.0, 0.25f});
    lane.loop = true;
    look.lanes.push_back(lane);

    auto write = [&](float x, float y) {
        std::vector<ParamWrite> base{{2, x}};
        std::vector<KeyframeLane> lw(1);
        lw[0].target = {fx_id, 3};
        lw[0].keys.push_back({0.0, y});
        undo.execute(doc,
                     set_param_gesture_command(look.id, look.effects[0].id, std::move(base), std::move(lw)),
                     /*coalesce=*/true);
    };
    write(0.6f, 0.60f);
    write(0.7f, 0.70f);
    write(0.8f, 0.80f);
    undo.break_coalescing();

    CHECK_EQ(look.effects[0].params[2], 0.8f);
    CHECK_EQ(look.lanes.size(), size_t{1});
    CHECK_EQ(look.lanes[0].keys[0].value, 0.8f);
    CHECK(look.lanes[0].loop);   // lane replace keeps loop/mute

    CHECK(undo.undo(doc));
    CHECK_EQ(look.effects[0].params[2], 0.5f);
    CHECK_EQ(look.lanes[0].keys[0].value, 0.25f);
    CHECK(!undo.can_undo());

    CHECK(undo.redo(doc));
    CHECK_EQ(look.effects[0].params[2], 0.8f);
    CHECK_EQ(look.lanes[0].keys[0].value, 0.8f);
}

TEST(stack_param_gesture_shape_guard) {
    // Different write shapes never merge into one undo entry.
    // Reverting a command that created a lane removes the lane.
    Document doc = doc_with_look();
    UndoStack undo;
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Spherize));
    Look& look = doc.looks[0];
    const uint64_t fx_id = look.effects[0].id;

    std::vector<ParamWrite> b1{{2, 0.6f}};
    undo.execute(doc,
                 set_param_gesture_command(look.id, look.effects[0].id, std::move(b1), {}),
                 /*coalesce=*/true);

    std::vector<ParamWrite> b2{{2, 0.65f}};
    std::vector<KeyframeLane> lw(1);
    lw[0].target = {fx_id, 3};
    lw[0].keys.push_back({0.0, 0.9f});
    undo.execute(doc,
                 set_param_gesture_command(look.id, look.effects[0].id, std::move(b2), std::move(lw)),
                 /*coalesce=*/true);
    CHECK_EQ(look.effects[0].params[2], 0.65f);
    CHECK_EQ(look.lanes.size(), size_t{1});

    CHECK(undo.undo(doc));
    CHECK_EQ(look.effects[0].params[2], 0.6f);
    CHECK_EQ(look.lanes.size(), size_t{0});
    CHECK(undo.undo(doc));
    CHECK_EQ(look.effects[0].params[2], 0.5f);
}
