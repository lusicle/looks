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
    // TRUE GRAPH (docs/flow_canvas.md): adjustment layers lost their
    // composite-tap special case — their effects wire like any node and
    // head at the shared source when unlinked.
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect && n.layer_index == 2)
            CHECK(g.nodes[static_cast<size_t>(n.inputs[0])].kind ==
                  GraphNode::Kind::Source);
    // Invisible layers compile out entirely.
    doc.layers[1].visible = false;
    RenderGraph g2 = compile_graph(doc);
    CHECK(g2.valid);
    int generators2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Generator) ++generators2;
    CHECK_EQ(generators2, 0);
}

TEST(graph_compile_dormant_unwired) {
    // v4: an effect with NO in-wire is DORMANT — never emitted, nothing
    // fabricated in its place — and an unwired Output composites nothing
    // (black display node, which no effect may consume as input).
    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.layers[0].stack[0].id;
    const uint64_t layer_id = doc.layers[0].id;
    // Explicit links: source -> output. The effect is NOT wired anywhere.
    doc.links.push_back({layer_id, 0, 0});

    RenderGraph g = compile_graph(doc);
    CHECK(g.valid);
    int effects = 0;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect) ++effects;
    CHECK_EQ(effects, 0);   // dormant: it must not process anything
    // Composite = the source head straight through.
    CHECK(g.nodes[static_cast<size_t>(g.output)].kind ==
          GraphNode::Kind::Source);

    // Wire the effect in: it emits and carries the composite.
    doc.links.clear();
    doc.links.push_back({layer_id, fx_id, 0});
    doc.links.push_back({fx_id, 0, 0});
    RenderGraph g2 = compile_graph(doc);
    CHECK(g2.valid);
    int effects2 = 0;
    for (const GraphNode& n : g2.nodes)
        if (n.kind == GraphNode::Kind::Effect) ++effects2;
    CHECK_EQ(effects2, 1);

    // Nothing wired to Output at all: the composite is the empty-display
    // generator — never the raw source.
    doc.links.clear();
    doc.links.push_back({layer_id, fx_id, 0});   // effect fed, not shown
    RenderGraph g3 = compile_graph(doc);
    CHECK(g3.valid);
    CHECK(g3.nodes[static_cast<size_t>(g3.output)].kind ==
          GraphNode::Kind::Generator);
    CHECK_EQ(g3.nodes[static_cast<size_t>(g3.output)].layer_index, -1);
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

TEST(graph_layer_matte_gates_composite) {
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    // Two visible layers; a Shape layer wired into the overlay's port 1
    // gates its whole contribution: the compile must wrap the overlay's
    // LayerBlend in a MatteApply whose base is the composite below.
    Document doc;
    Layer overlay;
    overlay.id = doc.next_effect_id++;
    overlay.source = LayerSourceKind::Noise;
    doc.layers.push_back(overlay);
    Layer matte;
    matte.id = doc.next_effect_id++;
    matte.source = LayerSourceKind::Shape;
    doc.layers.push_back(matte);
    doc.links.push_back({doc.layers[0].id, 0, 0});
    doc.links.push_back({overlay.id, 0, 0});
    doc.links.push_back({matte.id, overlay.id, 1});

    RenderGraph graph = compile_graph(doc);
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
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const uint64_t fx_id = doc.layers[0].stack[0].id;
    Layer matte;
    matte.id = doc.next_effect_id++;
    matte.source = LayerSourceKind::Shape;
    doc.layers.push_back(matte);
    doc.links.push_back({doc.layers[0].id, fx_id, 0});
    doc.links.push_back({fx_id, 0, 0});
    doc.links.push_back({matte.id, fx_id, 1});

    RenderGraph graph = compile_graph(doc);
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

TEST(graph_layer_transform_and_trim) {
    // Transform: a non-identity crop/flip/scale/rotate inserts a
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

TEST(graph_displace_by_matte_second_input) {
    using looks::doc::Layer;
    using looks::doc::LayerSourceKind;

    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Displace));
    const uint64_t fx_id = doc.layers[0].stack[0].id;
    Layer matte;
    matte.id = doc.next_effect_id++;
    matte.source = LayerSourceKind::Shape;
    doc.layers.push_back(matte);
    doc.links.push_back({doc.layers[0].id, fx_id, 0});
    doc.links.push_back({fx_id, 0, 0});
    doc.links.push_back({matte.id, fx_id, 1});

    // map_mode 0: the matte GATES (MatteApply join), displace has one
    // input.
    RenderGraph gated = compile_graph(doc);
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
    doc.layers[0].stack[0].params[3] = 1.0f;
    RenderGraph mapped = compile_graph(doc);
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
