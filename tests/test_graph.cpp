#include "gfx/graph.h"

#include <algorithm>
#include <cmath>

#include "doc/command.h"
#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/stack_commands.h"
#include "doc_fixture.h"
#include "test_framework.h"

using looks::doc::Asset;
using looks::doc::Document;
using looks::doc::EffectType;
using looks::doc::make_effect;
using looks::gfx::compile_graph;
using looks::gfx::GraphNode;
using looks::gfx::RenderGraph;
using looks::gfx::topo_sort;

namespace {

GraphNode node(std::initializer_list<int> inputs) {
    GraphNode n;
    n.kind = GraphNode::Kind::Effect;
    n.inputs = inputs;
    return n;
}

// A media node with an unregistered asset id is dormant.
uint64_t bind_asset(Document& doc, uint32_t frames = 600) {
    Asset a;
    a.id = doc.next_effect_id++;
    a.frame_count = frames;
    doc.assets.push_back(a);
    return a.id;
}

}  // namespace

TEST(graph_topo_linear_chain) {
    std::vector<GraphNode> nodes;
    nodes.push_back(node({2}));
    nodes.push_back(node({}));
    nodes.push_back(node({1}));
    std::vector<int> order;
    CHECK(topo_sort(nodes, order));
    CHECK_EQ(order.size(), size_t{3});
    CHECK_EQ(order[0], 1);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 0);
}

TEST(mode_analysis_uses_full_input_and_prunes_downstream) {
    auto doc = doc_with_look();
    doc.fps = 30;
    auto& look = doc.looks[0];
    look.sources[0].asset = bind_asset(doc, 900);
    look.duration = 300;
    look.trim_in = 120;
    look.trim_out = 180;
    auto before = make_effect(doc, EffectType::Invert);
    auto mode = make_effect(doc, EffectType::Mode);
    auto after = make_effect(doc, EffectType::Feedback);
    look.effects = {before, mode, after};
    look.links = {{look.sources[0].id, before.id, 0}, {before.id, mode.id, 0},
        {mode.id, after.id, 0}, {after.id, 0, 0}};
    CHECK_EQ(looks::gfx::mode_input_length(doc, look.id, mode.id), 900u);
    const auto samples = looks::gfx::mode_sample_frames(900, 64);
    CHECK_EQ(samples.front(), 0u);
    CHECK_EQ(samples.back(), 899u);
    CHECK_EQ(samples.size(), size_t{64});
    CHECK_EQ(looks::gfx::mode_sample_frames(2, 64).size(), size_t{2});
    auto graph = compile_graph(doc, look.id, 0, 0, 0, 0, false, mode.id);
    CHECK(graph.valid);
    int effects = 0;
    for (int index : graph.order)
        if (graph.nodes[index].kind == GraphNode::Kind::Effect) {
            CHECK_EQ(graph.nodes[index].effect_index, 0);
            ++effects;
        }
    CHECK_EQ(effects, 1);
    CHECK(graph.thumb_taps.empty());
    const auto signature = looks::gfx::mode_signature(doc, look.id, mode.id);
    look.effects[2].params[0] = 0.2f;
    look.effects[1].wet = 0.4f;
    look.effects[1].node_x = 600;
    CHECK_EQ(looks::gfx::mode_signature(doc, look.id, mode.id), signature);
    look.effects[0].wet = 0.5f;
    CHECK(looks::gfx::mode_signature(doc, look.id, mode.id) != signature);
}

TEST(mode_span_follows_offset_and_requires_a_finite_generator) {
    auto doc = doc_with_look();
    auto& look = doc.looks[0];
    const auto source = look.sources[0].id;
    look.sources[0].asset = bind_asset(doc, 900);
    auto offset = make_effect(doc, EffectType::Offset);
    auto mode = make_effect(doc, EffectType::Mode);
    look.effects = {offset, mode};
    look.links = {{source, offset.id, 0}, {offset.id, mode.id, 0}, {mode.id, 0, 0}};
    look.effects[0].params[0] = 100;
    CHECK_EQ(looks::gfx::mode_input_length(doc, look.id, mode.id), 800u);
    look.effects[0].params[0] = -100;
    CHECK_EQ(looks::gfx::mode_input_length(doc, look.id, mode.id), 1000u);
    look.effects[0].bypass = true;
    CHECK_EQ(looks::gfx::mode_input_length(doc, look.id, mode.id), 900u);
    look.sources[0].source = looks::doc::SourceKind::Solid;
    look.duration = 0;
    CHECK_EQ(looks::gfx::mode_input_length(doc, look.id, mode.id), 0u);
    look.duration = 30;
    CHECK_EQ(looks::gfx::mode_input_length(doc, look.id, mode.id), 30u);
}

TEST(mode_signature_tracks_group_matte_connection_settings) {
    auto doc = doc_with_look();
    auto& look = doc.looks[0];
    auto before = make_effect(doc, EffectType::Invert);
    auto mode = make_effect(doc, EffectType::Mode);
    looks::doc::Group group;
    group.id = doc.next_effect_id++;
    group.face_out = before.id;
    before.group_id = group.id;
    auto matte = looks::doc::make_source(doc, looks::doc::SourceKind::Solid);
    look.sources.push_back(matte);
    look.groups = {group};
    look.effects = {before, mode};
    look.links = {{look.sources[0].id, before.id, 0}, {before.id, mode.id, 0},
        {matte.id, group.id, 1}, {mode.id, 0, 0}};
    const auto signature = looks::gfx::mode_signature(doc, look.id, mode.id);
    look.links[2].blend = looks::doc::BlendMode::Multiply;
    CHECK(looks::gfx::mode_signature(doc, look.id, mode.id) != signature);
}

TEST(graph_preview_selection_does_not_change_render_demands) {
    Document doc = doc_with_look();
    doc.canvas_w = 640;
    doc.canvas_h = 480;
    auto& look = doc.looks[0];
    look.sources[0].source = looks::doc::SourceKind::Solid;
    const auto source = look.sources[0].id;
    auto other = looks::doc::make_source(doc, looks::doc::SourceKind::Shape);
    const auto effect = make_effect(doc, EffectType::Blur);
    look.effects.push_back(effect);
    look.sources.push_back(other);
    look.links = {{source, 0, 0}, {other.id, effect.id, 0}};
    const auto composite = compile_graph(doc, look.id, 0);
    const auto expected = looks::gfx::render_demands(doc, composite,
        looks::gfx::spatial_images(doc, composite), 640, 480);
    for (const auto selected : {source, other.id, effect.id}) {
        const auto graph = compile_graph(doc, look.id, 0, selected);
        CHECK(graph.valid);
        const auto demand = looks::gfx::render_demands(doc, graph,
            looks::gfx::spatial_images(doc, graph), 640, 480);
        CHECK(demand == expected);
        for (const auto& size : demand)
            if (size[0] > 0) CHECK(std::abs(size[0] / size[1] - 4.0 / 3.0) < 1e-6);
    }
}

TEST(graph_deleted_source_keeps_connected_effects_live) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto first = make_effect(doc, EffectType::Blur);
    look.effects.push_back(first);
    auto other = looks::doc::make_source(doc, looks::doc::SourceKind::Solid);
    look.sources.push_back(other);
    look.links = {{look.sources[0].id, first.id, 0}, {other.id, first.id, 0}, {first.id, 0, 0}};
    looks::doc::UndoStack undo;
    undo.execute(doc, looks::doc::remove_source_command(look.id, look.sources[0].id));
    auto graph = compile_graph(doc, look.id, 0);
    CHECK(graph.valid);
    bool found = false;
    for (const auto& n : graph.nodes)
        if (n.kind == GraphNode::Kind::Effect && n.effect_index == 0)
            found = true;
    CHECK(found);
}

TEST(graph_solo_uses_the_whole_look_and_ignores_bypassed_groups) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    look.sources[0].source = looks::doc::SourceKind::Solid;
    auto a = make_effect(doc, EffectType::Invert);
    auto b = make_effect(doc, EffectType::Blur);
    auto group = looks::doc::make_group(doc, "solo group");
    b.group_id = group.id;
    b.solo = true;
    group.face_out = b.id;
    look.groups.push_back(group);
    look.effects = {a, b};
    look.links = {{look.sources[0].id, a.id, 0}, {a.id, b.id, 0}, {b.id, 0, 0}};
    auto active = [&]() {
        const auto graph = compile_graph(doc, look.id, 0);
        CHECK(graph.valid);
        std::vector<int> ids;
        for (const auto& n : graph.nodes)
            if (n.kind == GraphNode::Kind::Effect) ids.push_back(n.effect_index);
        return ids;
    };
    CHECK(active() == std::vector<int>{1});
    look.groups[0].bypass = true;
    CHECK(active() == std::vector<int>{0});
    look.groups[0].bypass = false;
    look.effects[1].bypass = true;
    CHECK(active() == std::vector<int>{0});
    look.effects[1].bypass = false;
    look.effects[0].solo = true;
    CHECK(active() == (std::vector<int>{0, 1}));
}

TEST(graph_motion_uses_and_shares_the_upstream_image) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = looks::doc::SourceKind::Solid;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::RollingShutter));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Datamosh));
    look.links = {{layer.id, look.effects[0].id, 0},
        {look.effects[0].id, look.effects[1].id, 0}, {look.effects[1].id, 0, 0}};
    auto graph = compile_graph(doc, look.id, 0);
    CHECK(graph.valid);
    int flows = 0;
    for (const auto& n : graph.nodes) {
        if (n.kind == GraphNode::Kind::Flow) ++flows;
        if (n.kind == GraphNode::Kind::Effect) {
            CHECK_EQ(n.inputs.size(), size_t{2});
            const auto& flow = graph.nodes[static_cast<size_t>(n.inputs[1])];
            CHECK(flow.kind == GraphNode::Kind::Flow);
            CHECK_EQ(flow.inputs.size(), size_t{1});
            CHECK_EQ(flow.inputs[0], n.inputs[0]);
        }
    }
    CHECK_EQ(flows, 2);
    look.links = {{layer.id, doc.looks[0].effects[0].id, 0},
                  {layer.id, doc.looks[0].effects[1].id, 0},
                  {doc.looks[0].effects[0].id, 0, 0}, {doc.looks[0].effects[1].id, 0, 0}};
    graph = compile_graph(doc, look.id, 1);
    CHECK(graph.valid);
    flows = 0;
    for (const auto& n : graph.nodes)
        if (n.kind == GraphNode::Kind::Flow) ++flows;
    CHECK_EQ(flows, 1);
}

TEST(graph_topo_diamond) {
    std::vector<GraphNode> nodes;
    nodes.push_back(node({}));
    nodes.push_back(node({0}));
    nodes.push_back(node({0}));
    nodes.push_back(node({1, 2}));
    std::vector<int> order;
    CHECK(topo_sort(nodes, order));
    CHECK_EQ(order.size(), size_t{4});
    CHECK_EQ(order[0], 0);
    CHECK_EQ(order[3], 3);
}

TEST(graph_anaglyph_keeps_the_right_image_separate_from_the_matte) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    look.sources[0].source = looks::doc::SourceKind::Solid;
    look.effects.push_back(make_effect(doc, EffectType::Anaglyph));
    const auto left = look.sources[0].id;
    const auto fx = look.effects[0].id;
    looks::doc::Source right;
    right.id = doc.next_effect_id++;
    right.source = looks::doc::SourceKind::Gradient;
    look.sources.push_back(right);
    look.links = {{left, fx, 0}, {right.id, fx, 2}, {fx, 0, 0}};
    const auto graph = compile_graph(doc, look.id, 0);
    CHECK(graph.valid);
    int effects = 0;
    for (const auto& n : graph.nodes) {
        CHECK(n.kind != GraphNode::Kind::MatteExtract);
        if (n.kind != GraphNode::Kind::Effect) continue;
        ++effects;
        CHECK_EQ(n.inputs.size(), size_t{2});
        CHECK(n.inputs[0] != n.inputs[1]);
        CHECK_EQ(graph.nodes[static_cast<size_t>(n.inputs[1])].source_index, 1);
    }
    CHECK_EQ(effects, 1);
}

TEST(graph_jitter_camera_motion_is_independent_of_content) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    look.sources[0].source = looks::doc::SourceKind::Solid;
    look.effects.push_back(make_effect(doc, EffectType::Jitter));
    for (int mode = 0; mode < 4; ++mode) {
        look.effects[0].params[2] = static_cast<float>(mode);
        const auto graph = compile_graph(doc, look.id, 0);
        CHECK(graph.valid);
        int flows = 0;
        for (const auto& n : graph.nodes)
            if (n.kind == GraphNode::Kind::Flow) ++flows;
        CHECK_EQ(flows, 0);
    }
}

TEST(graph_topo_detects_cycle) {
    std::vector<GraphNode> nodes;
    nodes.push_back(node({1}));
    nodes.push_back(node({0}));
    std::vector<int> order;
    CHECK(!topo_sort(nodes, order));

    nodes.clear();
    nodes.push_back(node({0}));
    CHECK(!topo_sort(nodes, order));
}

TEST(graph_live_branch_survives_an_empty_peer) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = looks::doc::SourceKind::Solid;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Invert));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Blur));
    const uint64_t empty = doc.looks[0].effects[0].id, live = doc.looks[0].effects[1].id;
    look.links = {{layer.id, live, 0}, {empty, live, 0}, {live, 0, 0}};
    for (int order = 0; order < 2; ++order) {
        const auto g = compile_graph(doc, look.id, 0);
        CHECK(g.valid);
        CHECK(g.nodes[g.output].kind == GraphNode::Kind::Effect);
        if (g.nodes[g.output].kind == GraphNode::Kind::Effect)
            CHECK_EQ(look.effects[g.nodes[g.output].effect_index].id, live);
        std::reverse(doc.looks[0].effects.begin(), doc.looks[0].effects.end());
    }
}

TEST(graph_long_bypass_chain_keeps_its_source) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = looks::doc::SourceKind::Solid;
    for (int i = 0; i < 100; ++i) {
        doc.looks[0].effects.push_back(make_effect(doc, EffectType::Invert));
        doc.looks[0].effects.back().bypass = true;
    }
    look.links.clear();
    uint64_t previous = layer.id;
    for (const auto& fx : look.effects) {
        look.links.push_back({previous, fx.id, 0});
        previous = fx.id;
    }
    look.links.push_back({previous, 0, 0});
    const auto g = compile_graph(doc, look.id, 0);
    CHECK(g.valid);
    CHECK_EQ(g.nodes[g.output].source_index, 0);
}

TEST(graph_rejects_main_and_mask_cycles) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = looks::doc::SourceKind::Solid;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Invert));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Blur));
    const uint64_t a = doc.looks[0].effects[0].id, b = doc.looks[0].effects[1].id;
    look.links = {{a, b, 0}, {b, a, 0}, {a, 0, 0}};
    CHECK(!compile_graph(doc, look.id, 0).valid);
    look.links = {{layer.id, a, 0}, {a, layer.id, 1}, {a, 0, 0}};
    CHECK(!compile_graph(doc, look.id, 0).valid);
}

TEST(graph_group_mix_survives_a_bypassed_face) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = looks::doc::SourceKind::Solid;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Invert));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Blur));
    looks::doc::Group group;
    group.id = doc.next_effect_id++;
    group.inputs = {doc.next_effect_id++};
    group.face_out = doc.looks[0].effects[1].id;
    group.wet = 0.25f;
    doc.looks[0].groups.push_back(group);
    for (auto& fx : doc.looks[0].effects) fx.group_id = group.id;
    doc.looks[0].effects[1].bypass = true;
    look.links = {{layer.id, group.inputs[0], 0},
        {group.inputs[0], doc.looks[0].effects[0].id, 0},
        {doc.looks[0].effects[0].id, group.face_out, 0}, {group.face_out, 0, 0}};
    const auto g = compile_graph(doc, look.id, 0);
    CHECK(g.valid);
    CHECK(g.nodes[g.output].kind == GraphNode::Kind::GroupMix);
}

TEST(graph_offset_keeps_source_mask_and_multiple_inputs) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    look.sources[0].asset = bind_asset(doc);
    look.effects.push_back(make_effect(doc, EffectType::Offset));
    auto& offset = look.effects[0];
    offset.params[0] = 10.0f;
    offset.params[1] = 2.0f;
    const uint64_t source = look.sources[0].id, fx = offset.id;
    looks::doc::Source mask;
    mask.id = doc.next_effect_id++;
    mask.source = looks::doc::SourceKind::Solid;
    look.sources.push_back(mask);
    look.links = {{source, fx, 0}, {mask.id, source, 1}, {fx, 0, 0}};
    auto graph = compile_graph(doc, look.id, 20);
    CHECK(graph.valid);
    CHECK(graph.nodes[graph.output].kind == GraphNode::Kind::MatteApply);
    look.links = {{source, fx, 0}, {mask.id, fx, 0}, {fx, 0, 0}};
    graph = compile_graph(doc, look.id, 20);
    CHECK(graph.valid);
    CHECK(graph.nodes[graph.output].kind == GraphNode::Kind::LayerBlend);
    int sources = 0;
    for (const auto& n : graph.nodes)
        if (n.kind == GraphNode::Kind::Source) ++sources;
    CHECK_EQ(sources, 1);
}

TEST(graph_motion_separates_different_merged_inputs) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    look.sources[0].source = looks::doc::SourceKind::Solid;
    const uint64_t a = look.sources[0].id;
    looks::doc::Source second;
    second.id = doc.next_effect_id++;
    second.source = looks::doc::SourceKind::Gradient;
    look.effects.push_back(make_effect(doc, EffectType::RollingShutter));
    look.effects.push_back(make_effect(doc, EffectType::Datamosh));
    const uint64_t x = look.effects[0].id, y = look.effects[1].id;
    look.sources.push_back(second);
    look.links = {{a, x, 0}, {second.id, x, 0},
        {second.id, y, 0}, {a, y, 0}, {x, 0, 0}, {y, 0, 0}};
    const auto graph = compile_graph(doc, look.id, 0);
    CHECK(graph.valid);
    std::vector<uint64_t> keys;
    for (const auto& n : graph.nodes)
        if (n.kind == GraphNode::Kind::Flow) keys.push_back(n.key);
    CHECK_EQ(keys.size(), size_t{2});
    if (keys.size() == 2) CHECK(keys[0] != keys[1]);
}

TEST(graph_ports_keep_all_node_id_bits) {
    Document doc = doc_with_look();
    auto& look = doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = looks::doc::SourceKind::Solid;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Invert));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Blur));
    const uint64_t a = doc.looks[0].effects[0].id;
    doc.looks[0].effects[1].id = a + (uint64_t{1} << 56);
    const uint64_t b = doc.looks[0].effects[1].id;
    look.links = {{layer.id, a, 0}, {a, b, 0}, {b, 0, 0}};
    const auto graph = compile_graph(doc, look.id, 0);
    CHECK(graph.valid);
    CHECK_EQ(graph.nodes[graph.output].effect_index, 1);
    const int input = graph.nodes[graph.output].inputs[0];
    CHECK_EQ(graph.nodes[input].effect_index, 0);
}

TEST(graph_compile_empty_stack) {
    // Unbound media is dormant; source_index -1 is the black display generator.
    Document doc = doc_with_look();
    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    CHECK_EQ(graph.nodes.size(), size_t{1});
    CHECK(graph.nodes[0].kind == GraphNode::Kind::Generator);
    CHECK_EQ(graph.nodes[0].source_index, -1);

    doc.looks[0].sources[0].asset = bind_asset(doc);
    RenderGraph bound = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(bound.valid);
    CHECK(bound.nodes[static_cast<size_t>(bound.output)].kind ==
          GraphNode::Kind::Source);
}

TEST(graph_audio_effects_compile_out_of_the_image_graph) {
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::AudioDelay));
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Posterize));
    doc.looks[0].links = {{doc.looks[0].sources[0].id, doc.looks[0].effects[0].id, 0},
        {doc.looks[0].effects[0].id, doc.looks[0].effects[1].id, 0}, {doc.looks[0].effects[1].id, 0, 0}};
    RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int effects = 0;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect) {
            ++effects;
            CHECK(g.nodes[static_cast<size_t>(n.inputs[0])].kind ==
                  GraphNode::Kind::Source);
        }
    CHECK_EQ(effects, 1);
}

TEST(graph_compile_layers) {
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    looks::doc::Source overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = looks::doc::SourceKind::Noise;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Pixelate));
    doc.looks[0].sources.push_back(overlay);
    looks::doc::Source adjust;
    adjust.id = doc.next_effect_id++;
    adjust.source = looks::doc::SourceKind::Media;
    adjust.asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Grain));
    doc.looks[0].sources.push_back(adjust);
    doc.looks[0].links.clear();
    for (size_t i = 0; i < 3; ++i)
        connect_test_chain(doc.looks[0], doc.looks[0].sources[i].id, {doc.looks[0].effects[i].id});

    RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int generators = 0, blends = 0;
    for (const GraphNode& n : g.nodes) {
        if (n.kind == GraphNode::Kind::Generator) ++generators;
        if (n.kind == GraphNode::Kind::LayerBlend) ++blends;
    }
    CHECK_EQ(generators, 1);
    CHECK_EQ(blends, 2);
    const GraphNode& out = g.nodes[static_cast<size_t>(g.output)];
    CHECK(out.kind == GraphNode::Kind::LayerBlend);
    CHECK_EQ(out.source_index, -1);
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect && n.effect_index == 2)
            CHECK(g.nodes[static_cast<size_t>(n.inputs[0])].kind ==
                  GraphNode::Kind::Source);
    doc.looks[0].sources[1].visible = false;
    RenderGraph g2 = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g2.valid);
    int generators2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Generator) ++generators2;
    CHECK_EQ(generators2, 0);
}

TEST(graph_compile_dormant_unwired) {
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.looks[0].effects[0].id;
    const uint64_t layer_id = doc.looks[0].sources[0].id;
    doc.looks[0].links = {{layer_id, 0, 0}};

    RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int effects = 0;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect) ++effects;
    CHECK_EQ(effects, 0);
    CHECK(g.nodes[static_cast<size_t>(g.output)].kind ==
          GraphNode::Kind::Source);

    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({layer_id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    RenderGraph g2 = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g2.valid);
    int effects2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Effect) ++effects2;
    CHECK_EQ(effects2, 1);

    // With nothing wired to Output the composite is the empty display.
    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({layer_id, fx_id, 0});   // effect fed, not shown
    RenderGraph g3 = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g3.valid);
    CHECK(g3.nodes[static_cast<size_t>(g3.output)].kind ==
          GraphNode::Kind::Generator);
    CHECK_EQ(g3.nodes[static_cast<size_t>(g3.output)].source_index, -1);
}

TEST(graph_preview_source_taps_its_own_output) {
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Grain));
    const uint64_t base_id = doc.looks[0].sources[0].id;
    const uint64_t last_fx = doc.looks[0].effects[1].id;
    looks::doc::Source solid;
    solid.id = doc.next_effect_id++;
    solid.source = looks::doc::SourceKind::Solid;
    doc.looks[0].sources.push_back(solid);
    const uint64_t solid_id = solid.id;
    doc.looks[0].links.clear();
    connect_test_chain(doc.looks[0], base_id, {doc.looks[0].effects[0].id, last_fx});
    connect_test_chain(doc.looks[0], solid_id, {});
    RenderGraph by_node = compile_graph(doc, doc.looks[0].id, 0, last_fx);
    RenderGraph by_layer = compile_graph(doc, doc.looks[0].id, 0, 0, base_id);
    CHECK(by_node.valid);
    CHECK(by_node.preview >= 0);
    CHECK(by_layer.preview >= 0);
    CHECK_EQ(by_layer.nodes[by_layer.preview].kind, GraphNode::Kind::Source);
    CHECK_EQ(by_node.nodes[by_node.preview].effect_index, 1);
    // A bare layer's contribution IS its head: both taps agree there.
    RenderGraph solid_node = compile_graph(doc, doc.looks[0].id, 0, solid_id);
    RenderGraph solid_layer =
        compile_graph(doc, doc.looks[0].id, 0, 0, solid_id);
    CHECK(solid_node.preview >= 0);
    CHECK_EQ(solid_layer.preview, solid_node.preview);
    // The node tap outranks the layer tap when both are set.
    RenderGraph both =
        compile_graph(doc, doc.looks[0].id, 0, solid_id, base_id);
    CHECK_EQ(both.preview, solid_node.preview);
    // An id naming no layer resolves to nothing: the composite stays.
    RenderGraph none = compile_graph(doc, doc.looks[0].id, 0, 0, 99999);
    CHECK_EQ(none.preview, -1);
}

TEST(graph_preview_layer_resolves_a_mask_only_feed) {
    // The tap resolves the link leaving the layer, Output or not.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t base_id = doc.looks[0].sources[0].id;
    const uint64_t fx_id = doc.looks[0].effects[0].id;
    looks::doc::Source solid;
    solid.id = doc.next_effect_id++;
    solid.source = looks::doc::SourceKind::Solid;
    doc.looks[0].sources.push_back(solid);
    const uint64_t mask_id = solid.id;
    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({base_id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    doc.looks[0].links.push_back({mask_id, fx_id, 1});   // matte feed only

    RenderGraph by_layer = compile_graph(doc, doc.looks[0].id, 0, 0, mask_id);
    RenderGraph by_node = compile_graph(doc, doc.looks[0].id, 0, mask_id);
    CHECK(by_layer.valid);
    CHECK(by_layer.preview >= 0);
    CHECK_EQ(by_layer.preview, by_node.preview);
}

TEST(graph_compile_chain_and_bypass) {
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::RgbSplit));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Pixelate));
    doc.looks[0].effects[1].bypass = true;
    doc.looks[0].links.clear();
    connect_test_chain(doc.looks[0], doc.looks[0].sources[0].id,
        {doc.looks[0].effects[0].id, doc.looks[0].effects[1].id, doc.looks[0].effects[2].id});

    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    // Source + 2 live effects; the bypassed vignette is compiled out.
    CHECK_EQ(graph.nodes.size(), size_t{3});
    CHECK_EQ(graph.nodes[1].effect_index, 0);
    CHECK_EQ(graph.nodes[2].effect_index, 2);
    CHECK_EQ(graph.output, 2);
    CHECK_EQ(graph.nodes[1].inputs.size(), size_t{1});
    CHECK_EQ(graph.nodes[1].inputs[0], 0);
    CHECK_EQ(graph.nodes[2].inputs[0], 1);
    CHECK_EQ(graph.order.size(), size_t{3});
    CHECK_EQ(graph.order[0], 0);
    CHECK_EQ(graph.order[2], 2);
}

TEST(graph_layer_matte_gates_the_head) {
    using looks::doc::Source;
    using looks::doc::SourceKind;

    // A port-1 wire gates the layer HEAD, so its whole stack sees the crop.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    Source overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = SourceKind::Noise;
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.looks[0].effects[0].id;
    doc.looks[0].sources.push_back(overlay);
    Source matte;
    matte.id = doc.next_effect_id++;
    matte.source = SourceKind::Shape;
    doc.looks[0].sources.push_back(matte);
    doc.looks[0].links = {{doc.looks[0].sources[0].id, 0, 0}};
    doc.looks[0].links.push_back({overlay.id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    doc.looks[0].links.push_back({matte.id, overlay.id, 1});

    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    // The gate sits between the generator and the effect that reads it.
    int gated = -1;
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const GraphNode& n = graph.nodes[i];
        if (n.kind != GraphNode::Kind::MatteApply) continue;
        CHECK_EQ(n.inputs.size(), size_t{3});
        if (graph.nodes[static_cast<size_t>(n.inputs[1])].kind ==
            GraphNode::Kind::Generator)
            gated = static_cast<int>(i);
    }
    CHECK(gated >= 0);
    bool fx_reads_the_gate = false;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind != GraphNode::Kind::Effect) continue;
        for (int in : n.inputs)
            if (in == gated) fx_reads_the_gate = true;
    }
    CHECK(fx_reads_the_gate);
    // Nothing gates at the composite now: alpha-over reveals what is below.
    CHECK(graph.nodes[static_cast<size_t>(graph.output)].kind ==
          GraphNode::Kind::LayerBlend);
}

TEST(graph_effect_matte_diamond) {
    using looks::doc::Source;
    using looks::doc::SourceKind;

    // A port-1 wire on an effect gates it with an extract and an apply.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.looks[0].effects[0].id;
    Source matte;
    matte.id = doc.next_effect_id++;
    matte.source = SourceKind::Shape;
    doc.looks[0].sources.push_back(matte);
    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({doc.looks[0].sources[0].id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    doc.looks[0].links.push_back({matte.id, fx_id, 1});

    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    bool saw_extract = false, saw_apply = false;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind == GraphNode::Kind::MatteExtract) {
            saw_extract = true;
            CHECK_EQ(n.inputs.size(), size_t{1});
        }
        if (n.kind == GraphNode::Kind::MatteApply) {
            saw_apply = true;
            CHECK_EQ(n.inputs.size(), size_t{3});
        }
    }
    CHECK(saw_extract);
    CHECK(saw_apply);
    CHECK(graph.nodes[static_cast<size_t>(graph.output)].kind ==
          GraphNode::Kind::MatteApply);
}

TEST(graph_thumb_tap_follows_the_matte) {
    using looks::doc::Source;
    using looks::doc::SourceKind;

    // A card ends past its matte, so the thumbnail must tap the apply.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.looks[0].effects[0].id;
    const uint64_t src_id = doc.looks[0].sources[0].id;
    Source matte;
    matte.id = doc.next_effect_id++;
    matte.source = SourceKind::Shape;
    doc.looks[0].sources.push_back(matte);
    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({src_id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    doc.looks[0].links.push_back({matte.id, fx_id, 1});

    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    int fx_tap = -1, src_tap = -1, matte_tap = -1;
    for (const auto& t : graph.thumb_taps) {
        if (t.first == fx_id) fx_tap = t.second;
        if (t.first == (src_id | looks::gfx::kThumbSourceBit))
            src_tap = t.second;
        if (t.first == (matte.id | looks::gfx::kThumbSourceBit))
            matte_tap = t.second;
    }
    CHECK(fx_tap >= 0);
    CHECK_EQ(fx_tap, graph.output);
    CHECK(graph.nodes[static_cast<size_t>(fx_tap)].kind ==
          GraphNode::Kind::MatteApply);
    // Each source card still ends at its own head, matte included.
    CHECK(src_tap >= 0);
    CHECK(graph.nodes[static_cast<size_t>(src_tap)].kind ==
          GraphNode::Kind::Source);
    CHECK(matte_tap >= 0);
    CHECK(graph.nodes[static_cast<size_t>(matte_tap)].kind ==
          GraphNode::Kind::Generator);
}

TEST(graph_layer_transform_and_source_keys) {
    // A non-identity transform inserts a LayerTransform before the stack.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Vignette));
    doc.looks[0].links.clear();
    connect_test_chain(doc.looks[0], doc.looks[0].sources[0].id, {doc.looks[0].effects[0].id});

    RenderGraph plain = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(plain.valid);
    for (const GraphNode& n : plain.nodes)
        CHECK(n.kind != GraphNode::Kind::LayerTransform);
    int sources = 0;
    for (const GraphNode& n : plain.nodes)
        if (n.kind == GraphNode::Kind::Source) {
            ++sources;
            CHECK_EQ(n.source_index, 0);
            CHECK(n.key != 0);
        }
    CHECK_EQ(sources, 1);
    CHECK(plain.source >= 0);
    CHECK(plain.nodes[static_cast<size_t>(plain.source)].kind ==
          GraphNode::Kind::Source);

    doc.looks[0].sources[0].xf_rotate = 15.0f;
    RenderGraph xf = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(xf.valid);
    int transforms = 0;
    int source_node = -1;
    for (size_t i = 0; i < xf.nodes.size(); ++i)
        if (xf.nodes[i].kind == GraphNode::Kind::Source)
            source_node = static_cast<int>(i);
    for (const GraphNode& n : xf.nodes) {
        if (n.kind != GraphNode::Kind::LayerTransform) continue;
        ++transforms;
        CHECK_EQ(n.source_index, 0);
        CHECK_EQ(n.inputs.size(), size_t{1});
        CHECK_EQ(n.inputs[0], source_node);
    }
    CHECK_EQ(transforms, 1);
    // The stack effect reads the transformed source, not the raw one.
    for (const GraphNode& n : xf.nodes)
        if (n.kind == GraphNode::Kind::Effect)
            CHECK(n.inputs[0] != source_node);
}

TEST(graph_time_culled_matte_reads_as_closed_gate) {
    // A time-culled matte closes the gate to black, not to unwired.
    using looks::doc::Source;
    using looks::doc::SourceKind;

    Document doc = doc_with_look();
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.frame_count = 100;
    doc.assets.push_back(asset);
    doc.looks[0].sources[0].asset = asset.id;

    looks::doc::Look mask_look;
    mask_look.id = doc.next_effect_id++;
    mask_look.duration = 10;   // the mask ends at local 10
    Source shape;
    shape.id = doc.next_effect_id++;
    shape.source = SourceKind::Shape;
    mask_look.links.push_back({shape.id, 0, 0});
    mask_look.sources.push_back(std::move(shape));
    const uint64_t mask_look_id = mask_look.id;
    doc.looks.push_back(std::move(mask_look));

    Source mask;
    mask.id = doc.next_effect_id++;
    mask.source = SourceKind::LookRef;
    mask.target = mask_look_id;
    doc.looks[0].sources.push_back(mask);
    const uint64_t mask_id = doc.looks[0].sources.back().id;
    const uint64_t base_id = doc.looks[0].sources[0].id;
    doc.looks[0].links = {{base_id, 0, 0}};
    doc.looks[0].links.push_back({mask_id, base_id, 1});   // layer matte

    auto gate_feed = [&](uint32_t frame) -> int {
        // Returns -2 for no gate, else the feed generator's source_index.
        // Source index -1 is the black stand-in.
        const RenderGraph g = compile_graph(doc, doc.looks[0].id, frame);
        for (const GraphNode& n : g.nodes) {
            if (n.kind != GraphNode::Kind::MatteExtract) continue;
            const GraphNode& feed =
                g.nodes[static_cast<size_t>(n.inputs[0])];
            return feed.source_index;
        }
        return -2;
    };
    // Mask playing: the gate reads the nested look's shape (its layer 0).
    CHECK_EQ(gate_feed(5), 0);
    CHECK_EQ(gate_feed(20), -1);
}

TEST(graph_generator_has_no_when_but_a_placed_look_does) {
    // A generator is always on: only a placement gives it a time window.
    using looks::doc::Source;
    using looks::doc::SourceKind;

    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = SourceKind::Solid;

    auto gen_count = [&](uint64_t root, uint32_t frame) {
        const RenderGraph g = compile_graph(doc, root, frame);
        int n = 0;
        for (const GraphNode& node : g.nodes)
            if (node.kind == GraphNode::Kind::Generator &&
                node.source_index >= 0)
                ++n;
        return n;
    };
    CHECK_EQ(gen_count(doc.looks[0].id, 3), 1);
    CHECK_EQ(gen_count(doc.looks[0].id, 100000), 1);

    looks::doc::Placement wp;
    wp.id = doc.next_effect_id++;
    wp.target = doc.looks[0].id;
    wp.t_in = 4;
    wp.t_out = 8;
    doc.root().tracks[0].placements.push_back(wp);
    CHECK_EQ(gen_count(doc.root_sequence, 3), 0);
    CHECK_EQ(gen_count(doc.root_sequence, 4), 1);
    CHECK_EQ(gen_count(doc.root_sequence, 7), 1);
    CHECK_EQ(gen_count(doc.root_sequence, 8), 0);
}

TEST(graph_media_source_culled_past_its_media) {
    // Past the end of the media the source is culled.
    Document doc = doc_with_look();
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.frame_count = 10;
    doc.assets.push_back(asset);
    doc.looks[0].sources[0].asset = asset.id;

    auto source_count = [&](uint64_t root, uint32_t frame) {
        const RenderGraph g = compile_graph(doc, root, frame);
        int n = 0;
        for (const GraphNode& node : g.nodes)
            if (node.kind == GraphNode::Kind::Source) ++n;
        return n;
    };
    CHECK_EQ(source_count(doc.looks[0].id, 0), 1);
    CHECK_EQ(source_count(doc.looks[0].id, 9), 1);
    CHECK_EQ(source_count(doc.looks[0].id, 10), 0);
    // Slip shortens what remains of the media.
    doc.looks[0].sources[0].slip = 4;
    CHECK_EQ(source_count(doc.looks[0].id, 5), 1);
    CHECK_EQ(source_count(doc.looks[0].id, 6), 0);
    doc.looks[0].sources[0].slip = 0;

    looks::doc::Placement wp;
    wp.id = doc.next_effect_id++;
    wp.target = doc.looks[0].id;
    wp.t_in = 4;
    wp.t_out = 8;
    doc.root().tracks[0].placements.push_back(wp);
    CHECK_EQ(source_count(doc.root_sequence, 3), 0);
    CHECK_EQ(source_count(doc.root_sequence, 4), 1);
    CHECK_EQ(source_count(doc.root_sequence, 7), 1);
    CHECK_EQ(source_count(doc.root_sequence, 8), 0);
}

TEST(graph_placement_transform_and_opacity) {
    // The lane over-blend carries Motion, so no transform node appears.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement a;
    a.id = doc.next_effect_id++;
    a.target = doc.looks[0].id;
    a.pos_x = 0.25f;
    a.scale = 0.5f;
    a.rotate = 90.0f;
    a.opacity = 0.6f;
    a.anchor_x = 0.2f;
    a.anchor_y = 0.7f;
    seq.tracks[0].placements.push_back(a);

    RenderGraph g = compile_graph(doc, doc.root_sequence, 0);
    CHECK(g.valid);
    int blends = 0;
    for (const GraphNode& n : g.nodes) {
        CHECK(n.kind != GraphNode::Kind::LayerTransform);
        if (n.kind == GraphNode::Kind::LayerBlend) {
            ++blends;
            CHECK_EQ(n.source_index, -1);
            CHECK_EQ(n.p_shift_x, 0.25f);
            CHECK_EQ(n.p_scale, 0.5f);
            CHECK(std::fabs(n.p_rotate - 1.5707963f) < 1e-3f);
            CHECK_EQ(n.p_opacity, 0.6f);
            CHECK_EQ(n.p_anchor_x, 0.2f);
            CHECK_EQ(n.p_anchor_y, 0.7f);
        }
    }
    CHECK_EQ(blends, 1);

    // Identity transform at full opacity compiles to no blend at all.
    seq.tracks[0].placements[0] = [] {
        looks::doc::Placement p;
        return p;
    }();
    seq.tracks[0].placements[0].id = doc.next_effect_id++;
    seq.tracks[0].placements[0].target = doc.looks[0].id;
    RenderGraph plain = compile_graph(doc, doc.root_sequence, 0);
    CHECK(plain.valid);
    for (const GraphNode& n : plain.nodes) {
        CHECK(n.kind != GraphNode::Kind::LayerTransform);
        CHECK(n.kind != GraphNode::Kind::LayerBlend);
    }
}

TEST(graph_output_stacks_in_link_order) {
    // Stacking order is the link order, not the layer array order.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Source second;
    second.id = doc.next_effect_id++;
    second.source = looks::doc::SourceKind::Gradient;
    doc.looks[0].sources.push_back(second);
    const uint64_t l0 = doc.looks[0].sources[0].id;
    const uint64_t l1 = doc.looks[0].sources[1].id;
    doc.looks[0].links = {{l0, 0, 0}, {l1, 0, 0}};

    auto top_layer = [&]() -> int {
        const RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
        int li = -2;
        for (const GraphNode& n : g.nodes)
            if (n.kind == GraphNode::Kind::LayerBlend) li = g.nodes[n.inputs[1]].source_index;
        return li;
    };
    CHECK_EQ(top_layer(), 1);
    doc.looks[0].links = {{l1, 0, 0}, {l0, 0, 0}};
    CHECK_EQ(top_layer(), 0);

    // The permute command swaps the fan-in in place.
    looks::doc::UndoStack undo;
    undo.execute(doc, looks::doc::move_port_link_command(
                          doc.looks[0].id, 0, 0, 0, 1));
    CHECK_EQ(top_layer(), 1);
    CHECK(undo.undo(doc));
    CHECK_EQ(top_layer(), 0);
}

TEST(graph_bypassed_effect_thumbnail_uses_its_merged_input) {
    auto doc = doc_with_look();
    auto& look = doc.looks[0];
    look.sources[0].source = looks::doc::SourceKind::Solid;
    look.sources.push_back(looks::doc::make_source(doc, looks::doc::SourceKind::Noise));
    auto effect = make_effect(doc, EffectType::BlendNode);
    effect.bypass = true;
    look.effects.push_back(effect);
    look.links = {{look.sources[0].id, effect.id, 0},
        {look.sources[1].id, effect.id, 0, looks::doc::BlendMode::Screen}, {effect.id, 0, 0}};
    const auto graph = compile_graph(doc, look.id, 0);
    bool found = false;
    for (const auto& tap : graph.thumb_taps)
        if (tap.first == effect.id) {
            found = true;
            CHECK_EQ(tap.second, graph.output);
            CHECK(graph.nodes[tap.second].blend == looks::doc::BlendMode::Screen);
        }
    CHECK(found);
}

TEST(graph_effect_port_fan_in_merges_in_link_order) {
    // A fan-in port takes the composite, with the first link at the bottom.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Source second;
    second.id = doc.next_effect_id++;
    second.source = looks::doc::SourceKind::Gradient;
    doc.looks[0].sources.push_back(second);
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Blur));
    const uint64_t l0 = doc.looks[0].sources[0].id;
    const uint64_t l1 = doc.looks[0].sources[1].id;
    const uint64_t fx = doc.looks[0].effects[0].id;
    doc.looks[0].links = {{l0, fx, 0}, {l1, fx, 0, looks::doc::BlendMode::Multiply}, {fx, 0, 0}};

    const RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int fx_node = -1;
    for (size_t i = 0; i < g.nodes.size(); ++i)
        if (g.nodes[i].kind == GraphNode::Kind::Effect)
            fx_node = static_cast<int>(i);
    CHECK(fx_node >= 0);
    if (fx_node < 0) return;
    const int in = g.nodes[static_cast<size_t>(fx_node)].inputs[0];
    const GraphNode& merge = g.nodes[static_cast<size_t>(in)];
    CHECK(merge.kind == GraphNode::Kind::LayerBlend);
    CHECK(merge.blend == looks::doc::BlendMode::Multiply);
    CHECK_EQ(g.nodes[merge.inputs[1]].source_index, 1);
    const GraphNode& below = g.nodes[static_cast<size_t>(merge.inputs[0])];
    CHECK(below.kind == GraphNode::Kind::Generator);
    CHECK_EQ(below.source_index, 0);
}

TEST(graph_reconnect_lands_in_place) {
    // reconnect_command replaces a link at its position in the fan-in.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Source second;
    second.id = doc.next_effect_id++;
    second.source = looks::doc::SourceKind::Gradient;
    doc.looks[0].sources.push_back(second);
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Blur));
    const uint64_t l0 = doc.looks[0].sources[0].id;
    const uint64_t l1 = doc.looks[0].sources[1].id;
    const uint64_t fx = doc.looks[0].effects[0].id;
    doc.looks[0].links = {{l0, 0, 0}, {l1, 0, 0}};

    looks::doc::UndoStack undo;
    undo.execute(doc, looks::doc::reconnect_command(
                          doc.looks[0].id, {l0, 0, 0}, {fx, 0, 0}));
    CHECK_EQ(doc.looks[0].links[0].from, fx);
    CHECK_EQ(doc.looks[0].links[1].from, l1);
    CHECK(undo.undo(doc));
    CHECK_EQ(doc.looks[0].links[0].from, l0);
    CHECK_EQ(doc.looks[0].links[1].from, l1);
}

TEST(graph_placement_anchor_math) {
    // The forward map is out = a + shift + S*R*(src - a).
    // placement_uv_to_block is its exact inverse.
    const float aspect = 16.0f / 9.0f;
    looks::doc::Placement p;
    p.anchor_x = 0.2f;
    p.anchor_y = 0.7f;
    float bx = 0.0f, by = 0.0f;
    // Identity transform: any uv inverts to itself (block-local).
    looks::doc::placement_uv_to_block(p, 0.9f, 0.3f, aspect, &bx, &by);
    CHECK(std::fabs(bx - 0.4f) < 1e-5f);
    CHECK(std::fabs(by - (-0.2f)) < 1e-5f);

    p.scale = 0.5f;
    p.rotate = 33.0f;
    p.pos_x = 0.1f;
    p.pos_y = -0.05f;
    // The anchor is the fixed point: a + shift inverts back to the anchor.
    looks::doc::placement_uv_to_block(p, p.anchor_x + p.pos_x,
                                      p.anchor_y + p.pos_y, aspect, &bx,
                                      &by);
    CHECK(std::fabs(bx - (p.anchor_x - 0.5f)) < 1e-5f);
    CHECK(std::fabs(by - (p.anchor_y - 0.5f)) < 1e-5f);

    const float sx = 0.31f, sy = -0.12f;   // block-local
    const float rad = p.rotate * looks::doc::kDeg2Rad;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float ex = p.anchor_x - 0.5f, ey = p.anchor_y - 0.5f;
    const float qx = (sx - ex) * aspect, qy = sy - ey;
    const float u =
        0.5f + (qx * cs - qy * sn) * p.scale / aspect + ex + p.pos_x;
    const float v = 0.5f + (qx * sn + qy * cs) * p.scale + ey + p.pos_y;
    looks::doc::placement_uv_to_block(p, u, v, aspect, &bx, &by);
    CHECK(std::fabs(bx - sx) < 1e-5f);
    CHECK(std::fabs(by - sy) < 1e-5f);
}

TEST(graph_measure_taps_selected_block_pre_motion) {
    // The measure tap reads the block image before its placement Motion.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement a;
    a.id = doc.next_effect_id++;
    a.target = doc.looks[0].id;
    a.pos_x = 0.25f;
    a.scale = 0.5f;
    seq.tracks[0].placements.push_back(a);

    RenderGraph g = compile_graph(doc, doc.root_sequence, 0, 0, 0, a.id);
    CHECK(g.valid);
    CHECK(g.measure >= 0);
    // The transform node is the output here, so the tap differs from it.
    CHECK(g.nodes[static_cast<size_t>(g.measure)].kind !=
          GraphNode::Kind::LayerTransform);
    CHECK(g.measure != g.output);

    RenderGraph off = compile_graph(doc, doc.root_sequence, 0);
    CHECK_EQ(off.measure, -1);
    RenderGraph miss =
        compile_graph(doc, doc.root_sequence, 0, 0, 0, 0xDEADull);
    CHECK_EQ(miss.measure, -1);
}

TEST(graph_before_strips_effects_keeps_composition) {
    // The before tree keeps Motion and opacity but drops effect stacks.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Posterize));
    doc.looks[0].links = {{doc.looks[0].sources[0].id, doc.looks[0].effects[0].id, 0},
        {doc.looks[0].effects[0].id, 0, 0}};
    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement a;
    a.id = doc.next_effect_id++;
    a.target = doc.looks[0].id;
    a.pos_x = 0.25f;
    a.scale = 0.5f;
    seq.tracks[0].placements.push_back(a);

    RenderGraph g =
        compile_graph(doc, doc.root_sequence, 0, 0, 0, 0, true);
    CHECK(g.valid);
    CHECK(g.before >= 0);
    auto reach = [&](int root, auto&& visit) {
        std::vector<int> work = {root};
        std::vector<char> seen(g.nodes.size(), 0);
        while (!work.empty()) {
            const int idx = work.back();
            work.pop_back();
            if (seen[static_cast<size_t>(idx)]) continue;
            seen[static_cast<size_t>(idx)] = 1;
            visit(g.nodes[static_cast<size_t>(idx)]);
            for (int in : g.nodes[static_cast<size_t>(idx)].inputs)
                work.push_back(in);
        }
    };
    bool motion = false;
    reach(g.before, [&](const GraphNode& n) {
        CHECK(n.kind != GraphNode::Kind::Effect);
        if (n.kind == GraphNode::Kind::LayerBlend &&
            n.source_index == -1 && n.p_scale == 0.5f)
            motion = true;
    });
    CHECK(motion);
    bool fx = false;
    reach(g.output, [&](const GraphNode& n) {
        if (n.kind == GraphNode::Kind::Effect) fx = true;
    });
    CHECK(fx);
    // Default compile carries no before tree.
    RenderGraph off = compile_graph(doc, doc.root_sequence, 0);
    CHECK_EQ(off.before, -1);
}

TEST(graph_source_fit_rect_preserves_aspect) {
    // A source never stretches: it letterboxes or pillarboxes, centered.
    float r[4];
    looks::gfx::source_fit_rect(1920, 1080, 1920, 1080, r);
    CHECK_EQ(r[0], 0.0f);
    CHECK_EQ(r[1], 0.0f);
    CHECK_EQ(r[2], 1920.0f);
    CHECK_EQ(r[3], 1080.0f);

    looks::gfx::source_fit_rect(1920, 1080, 1080, 1080, r);   // wide in square
    CHECK_EQ(r[0], 0.0f);
    CHECK(std::fabs(r[2] - 1080.0f) < 1e-3f);
    CHECK(std::fabs(r[3] - 607.5f) < 1e-3f);
    CHECK(std::fabs(r[1] - (1080.0f - 607.5f) * 0.5f) < 1e-3f);

    looks::gfx::source_fit_rect(1080, 1920, 1920, 1080, r);   // tall in wide
    CHECK_EQ(r[1], 0.0f);
    CHECK(std::fabs(r[3] - 1080.0f) < 1e-3f);
    CHECK(std::fabs(r[2] - 1080.0f * 1080.0f / 1920.0f) < 1e-3f);
    CHECK(std::fabs(r[0] - (1920.0f - r[2]) * 0.5f) < 1e-3f);

    looks::gfx::source_fit_rect(0, 0, 640, 480, r);           // unknown dims
    CHECK_EQ(r[2], 640.0f);
    CHECK_EQ(r[3], 480.0f);
}

TEST(graph_sequence_lanes_stack_alpha_over) {
    // A lane blend has source_index -1 because no look supplies a mode.
    // One lane makes no blend node at all.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Look second;
    second.id = doc.next_effect_id++;
    looks::doc::Source noise;
    noise.id = doc.next_effect_id++;
    noise.source = looks::doc::SourceKind::Noise;
    second.sources.push_back(std::move(noise));
    second.links = {{second.sources[0].id, 0, 0}};
    const uint64_t second_id = second.id;
    doc.looks.push_back(std::move(second));

    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement a;
    a.id = doc.next_effect_id++;
    a.target = doc.looks[0].id;
    seq.tracks[0].placements.push_back(a);
    RenderGraph one = compile_graph(doc, doc.root_sequence, 0);
    CHECK(one.valid);
    for (const GraphNode& n : one.nodes)
        CHECK(n.kind != GraphNode::Kind::LayerBlend);

    looks::doc::SeqTrack lane2;
    lane2.id = doc.next_effect_id++;
    lane2.name = "v2";
    looks::doc::Placement b;
    b.id = doc.next_effect_id++;
    b.target = second_id;
    lane2.placements.push_back(b);
    seq.tracks.push_back(std::move(lane2));
    RenderGraph two = compile_graph(doc, doc.root_sequence, 0);
    CHECK(two.valid);
    int overs = 0;
    for (const GraphNode& n : two.nodes)
        if (n.kind == GraphNode::Kind::LayerBlend) {
            ++overs;
            CHECK_EQ(n.source_index, -1);
        }
    CHECK_EQ(overs, 1);

    // Overlap on ONE lane: the later-starting block wins the frame.
    seq.tracks.pop_back();
    looks::doc::Placement late;
    late.id = doc.next_effect_id++;
    late.target = second_id;
    late.t_in = 5;
    seq.tracks[0].placements.push_back(late);
    auto kinds_at = [&](uint32_t frame) {
        const RenderGraph g = compile_graph(doc, doc.root_sequence, frame);
        int solids = 0, noises = 0;
        for (const GraphNode& n : g.nodes) {
            if (n.kind != GraphNode::Kind::Generator || n.source_index < 0)
                continue;
            const looks::doc::Look& l = doc.look(
                g.instances[static_cast<size_t>(n.instance)].look);
            if (l.sources[static_cast<size_t>(n.source_index)].source ==
                looks::doc::SourceKind::Solid)
                ++solids;
            else
                ++noises;
        }
        return std::make_pair(solids, noises);
    };
    CHECK_EQ(kinds_at(3).first, 1);
    CHECK_EQ(kinds_at(3).second, 0);
    CHECK_EQ(kinds_at(6).first, 0);
    CHECK_EQ(kinds_at(6).second, 1);
}

TEST(graph_displace_by_matte_second_input) {
    using looks::doc::Source;
    using looks::doc::SourceKind;

    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    doc.looks[0].effects.push_back(make_effect(doc, EffectType::Displace));
    const uint64_t fx_id = doc.looks[0].effects[0].id;
    Source matte;
    matte.id = doc.next_effect_id++;
    matte.source = SourceKind::Shape;
    doc.looks[0].sources.push_back(matte);
    doc.looks[0].links.push_back({doc.looks[0].sources[0].id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    doc.looks[0].links.push_back({matte.id, fx_id, 1});

    // map_mode 0 makes the matte a gate, so displace keeps one input.
    RenderGraph gated = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(gated.valid);
    bool saw_apply = false;
    for (const GraphNode& n : gated.nodes) {
        if (n.kind == GraphNode::Kind::Effect)
            CHECK_EQ(n.inputs.size(), size_t{1});
        if (n.kind == GraphNode::Kind::MatteApply) saw_apply = true;
    }
    CHECK(saw_apply);

    // map_mode 1 makes the matte the displacement map on a second input.
    doc.looks[0].effects[0].params[3] = 1.0f;
    RenderGraph mapped = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(mapped.valid);
    bool saw_two_input_fx = false;
    for (const GraphNode& n : mapped.nodes) {
        CHECK(n.kind != GraphNode::Kind::MatteApply);
        if (n.kind == GraphNode::Kind::Effect &&
            n.inputs.size() == 2) {
            saw_two_input_fx = true;
            const GraphNode& map_node =
                mapped.nodes[static_cast<size_t>(n.inputs[1])];
            CHECK(map_node.kind == GraphNode::Kind::MatteExtract);
        }
    }
    CHECK(saw_two_input_fx);
}

namespace {

// The sorted keys are the fingerprint the engine hangs state on.
std::vector<uint64_t> state_fingerprint(const Document& doc, uint64_t root,
                                        uint32_t frame) {
    const RenderGraph g = compile_graph(doc, root, frame);
    std::vector<uint64_t> out;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Source ||
            n.kind == GraphNode::Kind::Effect)
            out.push_back(n.key);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(graph_razor_identity_is_structural) {
    // A cut must not change the state keys, so razored halves render alike.
    Document doc = doc_with_look();
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.frame_count = 200;
    doc.assets.push_back(asset);
    doc.looks[0].sources[0].asset = asset.id;
    doc.looks[0].effects.push_back(
        make_effect(doc, EffectType::Feedback));
    doc.looks[0].links = {{doc.looks[0].sources[0].id, doc.looks[0].effects[0].id, 0},
        {doc.looks[0].effects[0].id, 0, 0}};

    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement block;
    block.id = doc.next_effect_id++;
    block.target = doc.looks[0].id;
    seq.tracks[0].placements.push_back(block);
    const uint64_t lane_id = seq.tracks[0].id;

    std::vector<std::vector<uint64_t>> uncut;
    for (uint32_t f = 39; f <= 41; ++f)
        uncut.push_back(state_fingerprint(doc, doc.root_sequence, f));

    looks::doc::UndoStack undo;
    undo.execute(doc, looks::doc::razor_track_command(
                          doc, doc.root_sequence, lane_id, 40));
    CHECK_EQ(doc.root().tracks[0].placements.size(), size_t{2});
    for (uint32_t f = 39; f <= 41; ++f) {
        const auto cut = state_fingerprint(doc, doc.root_sequence, f);
        const auto& before = uncut[f - 39];
        CHECK_EQ(cut.size(), before.size());
        for (size_t i = 0; i < cut.size() && i < before.size(); ++i)
            CHECK_EQ(cut[i], before[i]);
    }
    // The same look on another lane gets its own keys.
    looks::doc::SeqTrack lane2;
    lane2.id = doc.next_effect_id++;
    looks::doc::Placement dup;
    dup.id = doc.next_effect_id++;
    dup.target = doc.looks[0].id;
    lane2.placements.push_back(dup);
    doc.root().tracks.push_back(std::move(lane2));
    const auto stacked = state_fingerprint(doc, doc.root_sequence, 39);
    CHECK_EQ(stacked.size(), uncut[0].size() * 2);
}

TEST(graph_glow_uses_one_effect_node_with_a_source_matte) {
    using looks::doc::Source;
    using looks::doc::SourceKind;

    Document doc = doc_with_look();
    doc.looks[0].sources[0].asset = bind_asset(doc);
    looks::doc::Look lab;
    lab.id = doc.next_effect_id++;
    lab.name = "matte lab";
    Source grad;
    grad.id = doc.next_effect_id++;
    grad.source = SourceKind::Gradient;
    lab.sources.push_back(grad);
    lab.effects.push_back(make_effect(doc, EffectType::Glow));
    const uint64_t glow_id = lab.effects[0].id;
    Source shape;
    shape.id = doc.next_effect_id++;
    shape.source = SourceKind::Shape;
    shape.osc_shape = 3;
    shape.path.resize(3);
    shape.path[0] = {0.5f, 0.2f, 0, 0, 0, 0};
    shape.path[1] = {0.8f, 0.8f, 0, 0, 0, 0};
    shape.path[2] = {0.2f, 0.8f, 0, 0, 0, 0};
    lab.sources.push_back(shape);
    lab.links.push_back({grad.id, glow_id, 0});
    lab.links.push_back({glow_id, 0, 0});
    lab.links.push_back({shape.id, glow_id, 1});
    doc.looks.push_back(lab);

    RenderGraph g = compile_graph(doc, lab.id, 0);
    CHECK(g.valid);
    int extract = -1, apply = -1;
    int generators = 0;
    int effects = 0;
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const GraphNode& n = g.nodes[i];
        for (int in : n.inputs) {
            CHECK(in >= 0);
            CHECK(in < static_cast<int>(i));   // inputs already emitted
        }
        if (n.kind == GraphNode::Kind::MatteExtract)
            extract = static_cast<int>(i);
        if (n.kind == GraphNode::Kind::MatteApply)
            apply = static_cast<int>(i);
        if (n.kind == GraphNode::Kind::Generator) ++generators;
        if (n.kind == GraphNode::Kind::Effect) ++effects;
    }
    CHECK(extract >= 0);
    CHECK(apply >= 0);
    CHECK_EQ(effects, 1);
    CHECK_EQ(generators, 2);   // gradient + shape, nothing fabricated
    const GraphNode& ex = g.nodes[static_cast<size_t>(extract)];
    CHECK_EQ(ex.inputs.size(), size_t{1});
    CHECK(g.nodes[static_cast<size_t>(ex.inputs[0])].kind ==
          GraphNode::Kind::Generator);
    // The apply joins (dry in, glow chain, gate).
    const GraphNode& ap = g.nodes[static_cast<size_t>(apply)];
    CHECK_EQ(ap.inputs.size(), size_t{3});
    CHECK_EQ(ap.inputs[2], extract);
}

TEST(graph_hidden_lane_leaves_the_composite) {
    // A hidden lane compiles as if it were not there.
    Document doc = doc_with_look();
    doc.looks[0].sources[0].source = looks::doc::SourceKind::Solid;
    looks::doc::Look second;
    second.id = doc.next_effect_id++;
    looks::doc::Source noise;
    noise.id = doc.next_effect_id++;
    noise.source = looks::doc::SourceKind::Noise;
    second.sources.push_back(std::move(noise));
    second.links = {{second.sources[0].id, 0, 0}};
    const uint64_t second_id = second.id;
    doc.looks.push_back(std::move(second));

    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement a;
    a.id = doc.next_effect_id++;
    a.target = doc.looks[0].id;
    seq.tracks[0].placements.push_back(a);
    looks::doc::SeqTrack lane2;
    lane2.id = doc.next_effect_id++;
    lane2.name = "v2";
    looks::doc::Placement b;
    b.id = doc.next_effect_id++;
    b.target = second_id;
    lane2.placements.push_back(b);
    seq.tracks.push_back(std::move(lane2));

    const RenderGraph both = compile_graph(doc, doc.root_sequence, 0);
    CHECK(both.valid);
    int overs = 0;
    for (const GraphNode& n : both.nodes)
        if (n.kind == GraphNode::Kind::LayerBlend) ++overs;
    CHECK_EQ(overs, 1);

    seq.tracks[1].hidden = true;
    const RenderGraph one = compile_graph(doc, doc.root_sequence, 0);
    CHECK(one.valid);
    for (const GraphNode& n : one.nodes)
        CHECK(n.kind != GraphNode::Kind::LayerBlend);
    CHECK_EQ(one.instances.size(), size_t{2});   // root + the bottom look
}
