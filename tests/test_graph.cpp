#include "gfx/graph.h"

#include <algorithm>

#include "doc/effects.h"
#include "doc/layer_commands.h"
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

// A registered asset for media binds: a media node naming an id the
// document cannot resolve is DORMANT (a removed import), so bound
// fixtures must register what they name.
uint64_t bind_asset(Document& doc, uint32_t frames = 600) {
    Asset a;
    a.id = doc.next_effect_id++;
    a.frame_count = frames;
    doc.assets.push_back(a);
    return a.id;
}

}  // namespace

TEST(graph_topo_linear_chain) {
    // 0 -> 1 -> 2, emitted out of order.
    std::vector<GraphNode> nodes;
    nodes.push_back(node({2}));    // 0 depends on 2
    nodes.push_back(node({}));     // 1 is the source
    nodes.push_back(node({1}));    // 2 depends on 1
    std::vector<int> order;
    CHECK(topo_sort(nodes, order));
    CHECK_EQ(order.size(), size_t{3});
    CHECK_EQ(order[0], 1);
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 0);
}

TEST(graph_topo_diamond) {
    // 0 source; 1 and 2 read 0; 3 reads both (matte-style join).
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

TEST(graph_topo_detects_cycle) {
    std::vector<GraphNode> nodes;
    nodes.push_back(node({1}));
    nodes.push_back(node({0}));
    std::vector<int> order;
    CHECK(!topo_sort(nodes, order));

    // Self-loop.
    nodes.clear();
    nodes.push_back(node({0}));
    CHECK(!topo_sort(nodes, order));
}

TEST(graph_compile_empty_stack) {
    // A fresh look holds one UNBOUND media node: dormant, so the
    // composite is the black display generator - never a fabricated
    // source. Binding a registered asset emits the Source head; an id
    // the document cannot resolve (a removed import) stays dormant.
    Document doc;
    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    CHECK_EQ(graph.nodes.size(), size_t{1});
    CHECK(graph.nodes[0].kind == GraphNode::Kind::Generator);
    CHECK_EQ(graph.nodes[0].layer_index, -1);

    doc.looks[0].layers[0].asset = bind_asset(doc);
    RenderGraph bound = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(bound.valid);
    CHECK(bound.nodes[static_cast<size_t>(bound.output)].kind ==
          GraphNode::Kind::Source);
}

TEST(graph_audio_effects_compile_out_of_the_image_graph) {
    // Audio modifiers are image-identity: the compiler routes the image
    // graph around them like bypassed nodes, so the video effect behind
    // one heads straight at the source.
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(
        make_effect(doc, EffectType::AudioDelay));
    doc.looks[0].layers[0].stack.push_back(
        make_effect(doc, EffectType::Posterize));
    RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int effects = 0;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect) {
            ++effects;
            CHECK(g.nodes[static_cast<size_t>(n.inputs[0])].kind ==
                  GraphNode::Kind::Source);
        }
    CHECK_EQ(effects, 1);   // posterize only - the delay never dispatches
}

TEST(graph_compile_layers) {
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    // A noise generator layer with its own effect, screened over the base.
    looks::doc::Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = looks::doc::LayerSourceKind::Noise;
    overlay.blend = looks::doc::BlendMode::Screen;
    overlay.stack.push_back(make_effect(doc, EffectType::Pixelate));
    doc.looks[0].layers.push_back(overlay);
    // A second media tap on top applying grain to the composite (the
    // adjustment kind died with the flat graph - a media tap merged back
    // IS an adjustment).
    looks::doc::Layer adjust;
    adjust.id = doc.next_effect_id++;
    adjust.source = looks::doc::LayerSourceKind::Media;
    adjust.asset = bind_asset(doc);
    adjust.stack.push_back(make_effect(doc, EffectType::Grain));
    doc.looks[0].layers.push_back(adjust);

    RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int generators = 0, blends = 0;
    for (const GraphNode& n : g.nodes) {
        if (n.kind == GraphNode::Kind::Generator) ++generators;
        if (n.kind == GraphNode::Kind::LayerBlend) ++blends;
    }
    CHECK_EQ(generators, 1);
    CHECK_EQ(blends, 2);   // overlay over base, adjustment over that
    // Output is the top blend; its layer_index is the adjustment layer.
    const GraphNode& out = g.nodes[static_cast<size_t>(g.output)];
    CHECK(out.kind == GraphNode::Kind::LayerBlend);
    CHECK_EQ(out.layer_index, 2);
    // TRUE GRAPH: a media tap's effects wire like
    // any node and head at its own source when unlinked.
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect && n.layer_index == 2)
            CHECK(g.nodes[static_cast<size_t>(n.inputs[0])].kind ==
                  GraphNode::Kind::Source);
    // Invisible layers compile out entirely.
    doc.looks[0].layers[1].visible = false;
    RenderGraph g2 = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g2.valid);
    int generators2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Generator) ++generators2;
    CHECK_EQ(generators2, 0);
}

TEST(graph_compile_dormant_unwired) {
    // An effect with NO in-wire is DORMANT — never emitted, nothing
    // fabricated in its place — and an unwired Output composites nothing
    // (black display node, which no effect may consume as input).
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.looks[0].layers[0].stack[0].id;
    const uint64_t layer_id = doc.looks[0].layers[0].id;
    // Explicit links: source -> output. The effect is NOT wired anywhere.
    doc.looks[0].links.push_back({layer_id, 0, 0});

    RenderGraph g = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g.valid);
    int effects = 0;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect) ++effects;
    CHECK_EQ(effects, 0);   // dormant: it must not process anything
    // Composite = the source head straight through.
    CHECK(g.nodes[static_cast<size_t>(g.output)].kind ==
          GraphNode::Kind::Source);

    // Wire the effect in: it emits and carries the composite.
    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({layer_id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    RenderGraph g2 = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g2.valid);
    int effects2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Effect) ++effects2;
    CHECK_EQ(effects2, 1);

    // Nothing wired to Output at all: the composite is the empty-display
    // generator — never the raw source.
    doc.looks[0].links.clear();
    doc.looks[0].links.push_back({layer_id, fx_id, 0});   // effect fed, not shown
    RenderGraph g3 = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(g3.valid);
    CHECK(g3.nodes[static_cast<size_t>(g3.output)].kind ==
          GraphNode::Kind::Generator);
    CHECK_EQ(g3.nodes[static_cast<size_t>(g3.output)].layer_index, -1);
}

TEST(graph_preview_layer_taps_the_chain_end) {
    // The LAYER tap publishes a layer's whole contribution - the end of
    // the chain leaving it, pre-blend - where the NODE tap on the same
    // layer id stays the bare source head. A node tap outranks it.
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Grain));
    const uint64_t base_id = doc.looks[0].layers[0].id;
    const uint64_t last_fx = doc.looks[0].layers[0].stack[1].id;
    looks::doc::Layer solid;
    solid.id = doc.next_effect_id++;
    solid.source = looks::doc::LayerSourceKind::Solid;
    doc.looks[0].layers.push_back(solid);
    const uint64_t solid_id = solid.id;

    // Synthesized wiring: the base chain ends at its last stack effect.
    RenderGraph by_node = compile_graph(doc, doc.looks[0].id, 0, last_fx);
    RenderGraph by_layer = compile_graph(doc, doc.looks[0].id, 0, 0, base_id);
    CHECK(by_node.valid);
    CHECK(by_node.preview >= 0);
    CHECK_EQ(by_layer.preview, by_node.preview);
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
    // A layer wired only as another chain's matte never feeds the
    // Output, but it still has an output of its own: the tap resolves
    // the link leaving the layer, Output or not.
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t base_id = doc.looks[0].layers[0].id;
    const uint64_t fx_id = doc.looks[0].layers[0].stack[0].id;
    looks::doc::Layer solid;
    solid.id = doc.next_effect_id++;
    solid.source = looks::doc::LayerSourceKind::Solid;
    doc.looks[0].layers.push_back(solid);
    const uint64_t mask_id = solid.id;
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
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::RgbSplit));
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Pixelate));
    doc.looks[0].layers[0].stack[1].bypass = true;

    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    // Source + 2 live effects; the bypassed vignette is compiled out.
    CHECK_EQ(graph.nodes.size(), size_t{3});
    CHECK_EQ(graph.nodes[1].effect_index, 0);
    CHECK_EQ(graph.nodes[2].effect_index, 2);
    CHECK_EQ(graph.output, 2);
    // Chain wiring: each effect reads its predecessor.
    CHECK_EQ(graph.nodes[1].inputs.size(), size_t{1});
    CHECK_EQ(graph.nodes[1].inputs[0], 0);
    CHECK_EQ(graph.nodes[2].inputs[0], 1);
    // Evaluation order follows the chain.
    CHECK_EQ(graph.order.size(), size_t{3});
    CHECK_EQ(graph.order[0], 0);
    CHECK_EQ(graph.order[2], 2);
}

TEST(graph_layer_matte_gates_composite) {
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    // Two visible layers; a Shape layer wired into the overlay's port 1
    // gates its whole contribution: the compile must wrap the overlay's
    // LayerBlend in a MatteApply whose base is the composite below.
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = LayerSourceKind::Noise;
    doc.looks[0].layers.push_back(overlay);
    Layer matte;
    matte.id = doc.next_effect_id++;
    matte.source = LayerSourceKind::Shape;
    doc.looks[0].layers.push_back(matte);
    doc.looks[0].links.push_back({doc.looks[0].layers[0].id, 0, 0});
    doc.looks[0].links.push_back({overlay.id, 0, 0});
    doc.looks[0].links.push_back({matte.id, overlay.id, 1});

    RenderGraph graph = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(graph.valid);
    bool saw_matted_blend = false;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind != GraphNode::Kind::MatteApply) continue;
        CHECK_EQ(n.inputs.size(), size_t{3});
        const GraphNode& fx =
            graph.nodes[static_cast<size_t>(n.inputs[1])];
        if (fx.kind == GraphNode::Kind::LayerBlend) saw_matted_blend = true;
    }
    CHECK(saw_matted_blend);
    // The matted blend is the graph output.
    CHECK(graph.nodes[static_cast<size_t>(graph.output)].kind ==
          GraphNode::Kind::MatteApply);
}

TEST(graph_effect_matte_diamond) {
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    // A port-1 wire on an effect gates it through extract + apply: the
    // apply joins (dry, fx, matte) and the matte source feeds ONLY the
    // gate — never the composite.
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.looks[0].layers[0].stack[0].id;
    Layer matte;
    matte.id = doc.next_effect_id++;
    matte.source = LayerSourceKind::Shape;
    doc.looks[0].layers.push_back(matte);
    doc.looks[0].links.push_back({doc.looks[0].layers[0].id, fx_id, 0});
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

TEST(graph_layer_transform_and_source_keys) {
    // Transform: a non-identity crop/flip/scale/rotate inserts a
    // LayerTransform between the layer source and its stack. Every media
    // source is private and keyed per instance - there is no shared
    // playhead source to fall back to.
    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));

    RenderGraph plain = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(plain.valid);
    for (const GraphNode& n : plain.nodes)
        CHECK(n.kind != GraphNode::Kind::LayerTransform);
    int sources = 0;
    for (const GraphNode& n : plain.nodes)
        if (n.kind == GraphNode::Kind::Source) {
            ++sources;
            CHECK_EQ(n.layer_index, 0);
            CHECK(n.key != 0);
        }
    CHECK_EQ(sources, 1);
    CHECK(plain.source >= 0);
    CHECK(plain.nodes[static_cast<size_t>(plain.source)].kind ==
          GraphNode::Kind::Source);

    doc.looks[0].layers[0].xf_rotate = 15.0f;
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
        CHECK_EQ(n.layer_index, 0);
        CHECK_EQ(n.inputs.size(), size_t{1});
        CHECK_EQ(n.inputs[0], source_node);   // reads its own source
    }
    CHECK_EQ(transforms, 1);
    // The stack effect reads the transformed source, not the raw one.
    for (const GraphNode& n : xf.nodes)
        if (n.kind == GraphNode::Kind::Effect)
            CHECK(n.inputs[0] != source_node);
}

TEST(graph_time_culled_matte_reads_as_closed_gate) {
    // Masking across time: while the mask's media plays, the masked
    // layer is gated by it; when it ENDS (an explicit duration on the
    // nested look), the gate closes (black) instead of the wire reading
    // as unwired — the masked contribution disappears, it does not pop
    // to full. A mask IS a nested look, playing lockstep.
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    Document doc;
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.frame_count = 100;
    doc.assets.push_back(asset);
    doc.looks[0].layers[0].asset = asset.id;

    looks::doc::Look mask_look;
    mask_look.id = doc.next_effect_id++;
    mask_look.duration = 10;   // the mask ends at local 10
    Layer shape;
    shape.id = doc.next_effect_id++;
    shape.source = LayerSourceKind::Shape;
    mask_look.layers.push_back(std::move(shape));
    const uint64_t mask_look_id = mask_look.id;
    doc.looks.push_back(std::move(mask_look));

    Layer mask;
    mask.id = doc.next_effect_id++;
    mask.source = LayerSourceKind::LookRef;
    mask.target = mask_look_id;
    doc.looks[0].layers.push_back(mask);
    const uint64_t mask_id = doc.looks[0].layers.back().id;
    const uint64_t base_id = doc.looks[0].layers[0].id;
    doc.looks[0].links.push_back({base_id, 0, 0});
    doc.looks[0].links.push_back({mask_id, base_id, 1});   // layer matte

    auto gate_feed = [&](uint32_t frame) -> int {
        // -2 no gate; else the layer_index of the generator feeding the
        // extract (-1 = the black stand-in).
        const RenderGraph g = compile_graph(doc, doc.looks[0].id, frame);
        for (const GraphNode& n : g.nodes) {
            if (n.kind != GraphNode::Kind::MatteExtract) continue;
            const GraphNode& feed =
                g.nodes[static_cast<size_t>(n.inputs[0])];
            return feed.layer_index;
        }
        return -2;
    };
    // Mask playing: the gate reads the nested look's shape (its layer 0).
    CHECK_EQ(gate_feed(5), 0);
    // Mask's duration over: the gate reads BLACK (closed), not unwired.
    CHECK_EQ(gate_feed(20), -1);
}

TEST(graph_generator_has_no_when_but_a_placed_look_does) {
    // Generators are always on - the SEQUENCE holds what has a when, the
    // graph holds the rest. A span comes from wrapping the generator in
    // a look and placing that as a block.
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    Document doc;
    doc.looks[0].layers[0].source = LayerSourceKind::Solid;

    auto gen_count = [&](uint64_t root, uint32_t frame) {
        const RenderGraph g = compile_graph(doc, root, frame);
        int n = 0;
        for (const GraphNode& node : g.nodes)
            if (node.kind == GraphNode::Kind::Generator &&
                node.layer_index >= 0)
                ++n;
        return n;
    };
    CHECK_EQ(gen_count(doc.looks[0].id, 3), 1);
    CHECK_EQ(gen_count(doc.looks[0].id, 100000), 1);

    // The same solid look placed at [4, 8) on the root sequence culls
    // outside the block.
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
    // A media node plays its media in lockstep from local 0: past the
    // media (through slip) the layer is a closed gate. On the sequence,
    // a block's own window culls the whole look outside it.
    Document doc;
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.frame_count = 10;
    doc.assets.push_back(asset);
    doc.looks[0].layers[0].asset = asset.id;

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
    doc.looks[0].layers[0].slip = 4;
    CHECK_EQ(source_count(doc.looks[0].id, 5), 1);
    CHECK_EQ(source_count(doc.looks[0].id, 6), 0);
    doc.looks[0].layers[0].slip = 0;

    // Placed at [4, 8): the sequence window culls outside the block.
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
    // Placement Motion is a COMPOSITION attribute: the lane's over-blend
    // carries the affine and opacity and samples through it while
    // compositing. No transform node ever appears in a sequence graph -
    // sequences own no effect passes. A bottom lane with Motion or
    // reduced opacity blends over transparent black so both are real.
    Document doc;
    doc.looks[0].layers[0].source = looks::doc::LayerSourceKind::Solid;
    looks::doc::Sequence& seq = doc.root();
    looks::doc::Placement a;
    a.id = doc.next_effect_id++;
    a.target = doc.looks[0].id;
    a.pos_x = 0.25f;
    a.scale = 0.5f;
    a.rotate = 90.0f;
    a.opacity = 0.6f;
    seq.tracks[0].placements.push_back(a);

    RenderGraph g = compile_graph(doc, doc.root_sequence, 0);
    CHECK(g.valid);
    int blends = 0;
    for (const GraphNode& n : g.nodes) {
        CHECK(n.kind != GraphNode::Kind::LayerTransform);
        if (n.kind == GraphNode::Kind::LayerBlend) {
            ++blends;
            CHECK_EQ(n.layer_index, -1);
            CHECK_EQ(n.p_shift_x, 0.25f);
            CHECK_EQ(n.p_scale, 0.5f);
            CHECK(std::fabs(n.p_rotate - 1.5707963f) < 1e-3f);
            CHECK_EQ(n.p_opacity, 0.6f);
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

TEST(graph_measure_taps_selected_block_pre_motion) {
    // The measure tap names the selected block's lane image BEFORE its
    // placement Motion: the monitor's box applies the transform itself,
    // so the measured bounds must be the untransformed content.
    Document doc;
    doc.looks[0].layers[0].source = looks::doc::LayerSourceKind::Solid;
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
    // Pre-Motion: the tap is not the transform node (which IS the
    // output here - one lane, full opacity).
    CHECK(g.nodes[static_cast<size_t>(g.measure)].kind !=
          GraphNode::Kind::LayerTransform);
    CHECK(g.measure != g.output);

    // No selection, no tap; an unknown id, no tap.
    RenderGraph off = compile_graph(doc, doc.root_sequence, 0);
    CHECK_EQ(off.measure, -1);
    RenderGraph miss =
        compile_graph(doc, doc.root_sequence, 0, 0, 0, 0xDEADull);
    CHECK_EQ(miss.measure, -1);
}

TEST(graph_before_strips_effects_keeps_composition) {
    // The A/B "before" is the same composition minus effect stacks:
    // arrangement, Motion and opacity are composition attributes and
    // survive the wipe; effects do not.
    Document doc;
    doc.looks[0].layers[0].source = looks::doc::LayerSourceKind::Solid;
    doc.looks[0].layers[0].stack.push_back(
        make_effect(doc, EffectType::Posterize));
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
            n.layer_index == -1 && n.p_scale == 0.5f)
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
    // Sources never stretch: matching aspects fill exactly (1:1 with
    // the old normalized sampling), mismatches letterbox/pillarbox
    // centered, unknown dims fill.
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
    // Lanes composite bottom-up with plain alpha-over: LayerBlend nodes
    // with layer_index -1 (no look supplies a mode). One lane = no blend
    // at all. Overlap within a lane shows the LATEST-STARTING block.
    Document doc;
    doc.looks[0].layers[0].source = looks::doc::LayerSourceKind::Solid;
    looks::doc::Look second;
    second.id = doc.next_effect_id++;
    looks::doc::Layer noise;
    noise.id = doc.next_effect_id++;
    noise.source = looks::doc::LayerSourceKind::Noise;
    second.layers.push_back(std::move(noise));
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
            CHECK_EQ(n.layer_index, -1);
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
            if (n.kind != GraphNode::Kind::Generator || n.layer_index < 0)
                continue;
            const looks::doc::Look& l = doc.look(
                g.instances[static_cast<size_t>(n.instance)].look);
            if (l.layers[static_cast<size_t>(n.layer_index)].source ==
                looks::doc::LayerSourceKind::Solid)
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
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    Document doc;
    doc.looks[0].layers[0].asset = bind_asset(doc);
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Displace));
    const uint64_t fx_id = doc.looks[0].layers[0].stack[0].id;
    Layer matte;
    matte.id = doc.next_effect_id++;
    matte.source = LayerSourceKind::Shape;
    doc.looks[0].layers.push_back(matte);
    doc.looks[0].links.push_back({doc.looks[0].layers[0].id, fx_id, 0});
    doc.looks[0].links.push_back({fx_id, 0, 0});
    doc.looks[0].links.push_back({matte.id, fx_id, 1});

    // map_mode 0: the matte GATES (MatteApply join), displace has one
    // input.
    RenderGraph gated = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(gated.valid);
    bool saw_apply = false;
    for (const GraphNode& n : gated.nodes) {
        if (n.kind == GraphNode::Kind::Effect)
            CHECK_EQ(n.inputs.size(), size_t{1});
        if (n.kind == GraphNode::Kind::MatteApply) saw_apply = true;
    }
    CHECK(saw_apply);

    // map_mode 1: the matte becomes the displacement MAP — the effect
    // node gains it as a second input and no MatteApply gate is emitted.
    doc.looks[0].layers[0].stack[0].params[3] = 1.0f;
    RenderGraph mapped = compile_graph(doc, doc.looks[0].id, 0);
    CHECK(mapped.valid);
    bool saw_two_input_fx = false;
    for (const GraphNode& n : mapped.nodes) {
        CHECK(n.kind != GraphNode::Kind::MatteApply);
        if (n.kind == GraphNode::Kind::Effect &&
            n.inputs.size() == 2) {
            saw_two_input_fx = true;
            // Input 1 is the matte gate (extract node).
            const GraphNode& map_node =
                mapped.nodes[static_cast<size_t>(n.inputs[1])];
            CHECK(map_node.kind == GraphNode::Kind::MatteExtract);
        }
    }
    CHECK(saw_two_input_fx);
}

namespace {

// Every key the compile emits for Source and Effect nodes, sorted - the
// compile-level fingerprint the engine keys its state on.
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
    // RAZOR IDENTITY: cutting a block and butting the halves back
    // together renders bit-identically - the state keys the engine hangs
    // history on are UNCHANGED by the cut, and with zero effects at
    // sequence level there is nothing a cut could reset. A cut is a
    // window onto the target; the target cannot see it.
    Document doc;
    Asset asset;
    asset.id = doc.next_effect_id++;
    asset.frame_count = 200;
    doc.assets.push_back(asset);
    doc.looks[0].layers[0].asset = asset.id;
    doc.looks[0].layers[0].stack.push_back(
        make_effect(doc, EffectType::Feedback));

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
    // Duplicating the look on ANOTHER lane gets its OWN keys: instances
    // stay distinct while razored halves share.
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
