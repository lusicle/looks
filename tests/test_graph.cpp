#include "gfx/graph.h"

#include "doc/effects.h"
#include "test_framework.h"

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
    // 0 source; 1 and 2 read 0; 3 reads both (mask-style join).
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
    Document doc;
    RenderGraph graph = compile_graph(doc);
    CHECK(graph.valid);
    CHECK_EQ(graph.nodes.size(), size_t{1});
    CHECK_EQ(graph.output, 0);
    CHECK(graph.nodes[0].kind == GraphNode::Kind::Source);
}

TEST(graph_compile_layers) {
    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    // A noise generator layer with its own effect, screened over the base.
    looks::doc::Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = looks::doc::LayerSourceKind::Noise;
    overlay.blend = looks::doc::BlendMode::Screen;
    overlay.stack.push_back(make_effect(doc, EffectType::Pixelate));
    doc.layers.push_back(overlay);
    // An adjustment layer on top applying grain to the composite.
    looks::doc::Layer adjust;
    adjust.id = doc.next_effect_id++;
    adjust.source = looks::doc::LayerSourceKind::Adjustment;
    adjust.stack.push_back(make_effect(doc, EffectType::Grain));
    doc.layers.push_back(adjust);

    RenderGraph g = compile_graph(doc);
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
    // The adjustment's grain effect reads the overlay blend (the composite
    // below), not the raw source.
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect && n.layer_index == 2)
            CHECK(g.nodes[static_cast<size_t>(n.inputs[0])].kind ==
                  GraphNode::Kind::LayerBlend);
    // Invisible layers compile out entirely.
    doc.layers[1].visible = false;
    RenderGraph g2 = compile_graph(doc);
    CHECK(g2.valid);
    int generators2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Generator) ++generators2;
    CHECK_EQ(generators2, 0);
}

TEST(graph_compile_chain_and_bypass) {
    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::RgbSplit));
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Pixelate));
    doc.layers[0].stack[1].bypass = true;

    RenderGraph graph = compile_graph(doc);
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

TEST(graph_layer_mask_gates_composite) {
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;
    using looks::doc::Mask;
    using looks::doc::MaskType;

    // Two layers; the overlay carries a LAYER mask (spec §8): the compile
    // must wrap its LayerBlend in a MaskApply whose base is the composite
    // below.
    Document doc;
    Mask mask;
    mask.id = doc.next_mask_id++;
    mask.type = MaskType::Shape;
    doc.masks.push_back(mask);
    Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = LayerSourceKind::Noise;
    overlay.mask_id = mask.id;
    doc.layers.push_back(overlay);

    RenderGraph graph = compile_graph(doc);
    CHECK(graph.valid);
    bool saw_masked_blend = false;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind != GraphNode::Kind::MaskApply) continue;
        CHECK_EQ(n.inputs.size(), size_t{3});
        const GraphNode& fx =
            graph.nodes[static_cast<size_t>(n.inputs[1])];
        if (fx.kind == GraphNode::Kind::LayerBlend) saw_masked_blend = true;
    }
    CHECK(saw_masked_blend);
    // The masked blend is the graph output.
    CHECK(graph.nodes[static_cast<size_t>(graph.output)].kind ==
          GraphNode::Kind::MaskApply);
}

TEST(graph_mask_motion_combine_morph) {
    using looks::doc::Mask;
    using looks::doc::MaskCombineOp;
    using looks::doc::MaskType;

    // Motion mask (spec §8): reads the shared flow field through an
    // extract node; combine folds a second mask in through a MaskCombine
    // node; grow adds the separable morph pair; combine cycles (A<->B)
    // must terminate and stay acyclic.
    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));

    Mask motion;
    motion.id = doc.next_mask_id++;
    motion.type = MaskType::Motion;
    motion.grow_px = 8.0f;
    doc.masks.push_back(motion);

    Mask shape;
    shape.id = doc.next_mask_id++;
    shape.type = MaskType::Shape;
    doc.masks.push_back(shape);

    doc.masks[0].combine_id = shape.id;
    doc.masks[0].combine_op = MaskCombineOp::Intersect;
    doc.masks[1].combine_id = motion.id;   // cycle back — must be ignored
    doc.layers[0].stack[0].mask_id = motion.id;

    RenderGraph graph = compile_graph(doc);
    CHECK(graph.valid);
    bool saw_flow = false, saw_combine = false;
    int morphs = 0;
    for (const GraphNode& n : graph.nodes) {
        if (n.kind == GraphNode::Kind::Flow) saw_flow = true;
        if (n.kind == GraphNode::Kind::MaskCombine) {
            saw_combine = true;
            CHECK_EQ(n.inputs.size(), size_t{2});
        }
        if (n.kind == GraphNode::Kind::MaskMorphH ||
            n.kind == GraphNode::Kind::MaskMorphV)
            ++morphs;
    }
    CHECK(saw_flow);
    CHECK(saw_combine);
    CHECK_EQ(morphs, 2);
}

TEST(graph_mask_generator_and_layer_source) {
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;
    using looks::doc::Mask;
    using looks::doc::MaskType;

    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    Layer noise;
    noise.id = doc.next_effect_id++;
    noise.source = LayerSourceKind::Noise;
    doc.layers.push_back(noise);

    // Generator-sourced mask: a Generator node owned by the mask (no
    // layer_index).
    Mask gen_mask;
    gen_mask.id = doc.next_mask_id++;
    gen_mask.type = MaskType::Luma;
    gen_mask.source_gen = static_cast<uint32_t>(LayerSourceKind::Noise);
    doc.masks.push_back(gen_mask);
    doc.layers[0].stack[0].mask_id = gen_mask.id;

    RenderGraph g1 = compile_graph(doc);
    CHECK(g1.valid);
    bool saw_mask_gen = false;
    for (const GraphNode& n : g1.nodes)
        if (n.kind == GraphNode::Kind::Generator && n.layer_index < 0 &&
            n.mask_index >= 0)
            saw_mask_gen = true;
    CHECK(saw_mask_gen);

    // Layer-sourced mask: reuses the noise layer's Generator as source.
    doc.masks[0].source_gen = 0;
    doc.masks[0].source_layer_id = noise.id;
    RenderGraph g2 = compile_graph(doc);
    CHECK(g2.valid);
    int layer_gens = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Generator && n.layer_index == 1)
            ++layer_gens;
    // One for the layer itself, one feeding the mask.
    CHECK_EQ(layer_gens, 2);
}

TEST(graph_layer_transform_and_trim) {
    // Transform (spec §5): a non-identity crop/flip/scale/rotate inserts a
    // LayerTransform between the layer source and its stack; trim swaps the
    // shared playhead source for a private per-layer Source node.
    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));

    RenderGraph plain = compile_graph(doc);
    CHECK(plain.valid);
    for (const GraphNode& n : plain.nodes)
        CHECK(n.kind != GraphNode::Kind::LayerTransform);

    doc.layers[0].xf_rotate = 15.0f;
    RenderGraph xf = compile_graph(doc);
    CHECK(xf.valid);
    int transforms = 0;
    for (const GraphNode& n : xf.nodes) {
        if (n.kind != GraphNode::Kind::LayerTransform) continue;
        ++transforms;
        CHECK_EQ(n.layer_index, 0);
        CHECK_EQ(n.inputs.size(), size_t{1});
        CHECK_EQ(n.inputs[0], 0);   // reads the shared source
    }
    CHECK_EQ(transforms, 1);
    // The stack effect reads the transformed source, not node 0.
    for (const GraphNode& n : xf.nodes)
        if (n.kind == GraphNode::Kind::Effect)
            CHECK(n.inputs[0] != 0);

    doc.layers[0].xf_rotate = 0.0f;
    doc.layers[0].trim_in = 12;
    RenderGraph trimmed = compile_graph(doc);
    CHECK(trimmed.valid);
    // Node 0 stays (shared playhead source); the layer gets a private one.
    int private_sources = 0;
    for (size_t i = 1; i < trimmed.nodes.size(); ++i)
        if (trimmed.nodes[i].kind == GraphNode::Kind::Source &&
            trimmed.nodes[i].layer_index == 0)
            ++private_sources;
    CHECK_EQ(private_sources, 1);
}

TEST(graph_displace_by_mask_second_input) {
    using looks::doc::Mask;
    using looks::doc::MaskType;

    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Displace));
    Mask mask;
    mask.id = doc.next_mask_id++;
    mask.type = MaskType::Luma;
    doc.masks.push_back(mask);
    doc.layers[0].stack[0].mask_id = mask.id;

    // map_mode 0: the mask GATES (MaskApply join), displace has one input.
    RenderGraph gated = compile_graph(doc);
    CHECK(gated.valid);
    bool saw_apply = false;
    for (const GraphNode& n : gated.nodes) {
        if (n.kind == GraphNode::Kind::Effect)
            CHECK_EQ(n.inputs.size(), size_t{1});
        if (n.kind == GraphNode::Kind::MaskApply) saw_apply = true;
    }
    CHECK(saw_apply);

    // map_mode 1: the mask becomes the displacement MAP — the effect node
    // gains it as a second input and no MaskApply gate is emitted.
    doc.layers[0].stack[0].params[3] = 1.0f;
    RenderGraph mapped = compile_graph(doc);
    CHECK(mapped.valid);
    bool saw_two_input_fx = false;
    for (const GraphNode& n : mapped.nodes) {
        CHECK(n.kind != GraphNode::Kind::MaskApply);
        if (n.kind == GraphNode::Kind::Effect &&
            n.inputs.size() == 2) {
            saw_two_input_fx = true;
            // Input 1 is the mask subgraph's output (extract node).
            const GraphNode& map_node =
                mapped.nodes[static_cast<size_t>(n.inputs[1])];
            CHECK(map_node.kind == GraphNode::Kind::MaskExtract);
        }
    }
    CHECK(saw_two_input_fx);
}
