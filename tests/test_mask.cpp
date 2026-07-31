#include "doc/mask_commands.h"

#include "doc/effects.h"
#include "gfx/graph.h"
#include "test_framework.h"

using namespace looks;
using gfx::GraphNode;
using gfx::RenderGraph;

namespace {

doc::Document make_doc() {
    doc::Document d;
    d.layers[0].stack.push_back(doc::make_effect(d, doc::EffectType::Vignette));
    d.layers[0].stack.push_back(doc::make_effect(d, doc::EffectType::Pixelate));
    return d;
}

doc::Mask make_mask(doc::Document& d, doc::MaskType type) {
    doc::Mask m;
    m.id = d.next_mask_id++;
    m.name = "mask" + std::to_string(m.id);
    m.type = type;
    return m;
}

int count_kind(const RenderGraph& g, GraphNode::Kind kind) {
    int n = 0;
    for (const GraphNode& node : g.nodes)
        if (node.kind == kind) ++n;
    return n;
}

}  // namespace

TEST(mask_commands_lifecycle) {
    doc::Document d = make_doc();
    doc::UndoStack undo;

    undo.execute(d, doc::add_mask_command(make_mask(d, doc::MaskType::Shape)));
    CHECK_EQ(d.masks.size(), size_t{1});
    const uint64_t mask_id = d.masks[0].id;

    undo.execute(d, doc::set_effect_mask_command(0, 0, mask_id));
    CHECK_EQ(d.layers[0].stack[0].mask_id, mask_id);

    // Param edit coalesces per mask.
    doc::Mask edited = d.masks[0];
    edited.feather = 0.2f;
    undo.execute(d, doc::set_mask_params_command(edited), true);
    edited.feather = 0.3f;
    undo.execute(d, doc::set_mask_params_command(edited), true);
    CHECK_EQ(d.masks[0].feather, 0.3f);
    undo.undo(d);
    CHECK_EQ(d.masks[0].feather, 0.05f);
    undo.redo(d);

    // Removing the mask clears references; undo restores both.
    undo.execute(d, doc::remove_mask_command(mask_id));
    CHECK(d.masks.empty());
    CHECK_EQ(d.layers[0].stack[0].mask_id, uint64_t{0});
    undo.undo(d);
    CHECK_EQ(d.masks.size(), size_t{1});
    CHECK_EQ(d.layers[0].stack[0].mask_id, mask_id);
}

TEST(mask_chain_commands) {
    doc::Document d = make_doc();
    doc::UndoStack undo;
    undo.execute(d, doc::add_mask_command(make_mask(d, doc::MaskType::Luma)));
    const uint64_t mask_id = d.masks[0].id;

    undo.execute(d, doc::mask_chain_add_command(
                        mask_id, doc::make_effect(d, doc::EffectType::Pixelate)));
    CHECK_EQ(d.masks[0].chain.size(), size_t{1});

    undo.execute(d, doc::mask_chain_set_param_command(mask_id, 0, 0, 32.0f),
                 true);
    undo.execute(d, doc::mask_chain_set_param_command(mask_id, 0, 0, 48.0f),
                 true);
    CHECK_EQ(d.masks[0].chain[0].params[0], 48.0f);
    undo.undo(d);   // coalesced drag
    CHECK_EQ(d.masks[0].chain[0].params[0], 12.0f);
    undo.redo(d);

    undo.execute(d, doc::mask_chain_remove_command(mask_id, 0));
    CHECK(d.masks[0].chain.empty());
    undo.undo(d);
    CHECK_EQ(d.masks[0].chain.size(), size_t{1});
    CHECK_EQ(d.masks[0].chain[0].params[0], 48.0f);
}

TEST(graph_masked_effect_diamond) {
    doc::Document d = make_doc();
    doc::Mask mask = make_mask(d, doc::MaskType::Shape);
    d.masks.push_back(mask);
    d.layers[0].stack[0].mask_id = mask.id;

    RenderGraph g = gfx::compile_graph(d);
    CHECK(g.valid);
    // source, fx0, shape, apply, fx1.
    CHECK_EQ(g.nodes.size(), size_t{5});
    CHECK_EQ(count_kind(g, GraphNode::Kind::MaskShape), 1);
    CHECK_EQ(count_kind(g, GraphNode::Kind::MaskApply), 1);

    // The apply node reads dry (source), fx, and mask — the diamond.
    int apply = -1;
    for (size_t i = 0; i < g.nodes.size(); ++i)
        if (g.nodes[i].kind == GraphNode::Kind::MaskApply)
            apply = static_cast<int>(i);
    CHECK(apply >= 0);
    CHECK_EQ(g.nodes[apply].inputs.size(), size_t{3});
    CHECK_EQ(g.nodes[apply].inputs[0], 0);   // dry = source
    // The final effect consumes the apply output.
    CHECK_EQ(g.nodes[static_cast<size_t>(g.output)].effect_index, 1);
    CHECK_EQ(g.nodes[static_cast<size_t>(g.output)].inputs[0], apply);
}

TEST(graph_mask_chain_and_sharing) {
    doc::Document d = make_doc();
    doc::Mask mask = make_mask(d, doc::MaskType::Luma);
    mask.blur_px = 4.0f;
    mask.chain.push_back(doc::make_effect(d, doc::EffectType::Pixelate));
    d.masks.push_back(mask);
    // BOTH effects share the mask — subgraph must compile once.
    d.layers[0].stack[0].mask_id = mask.id;
    d.layers[0].stack[1].mask_id = mask.id;

    RenderGraph g = gfx::compile_graph(d);
    CHECK(g.valid);
    CHECK_EQ(count_kind(g, GraphNode::Kind::MaskExtract), 1);
    CHECK_EQ(count_kind(g, GraphNode::Kind::MaskBlurH), 1);
    CHECK_EQ(count_kind(g, GraphNode::Kind::MaskBlurV), 1);
    CHECK_EQ(count_kind(g, GraphNode::Kind::MaskApply), 2);
    // Chain effect: an Effect node with mask/chain indices set.
    int chain_effects = 0;
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::Effect && n.chain_index >= 0) {
            ++chain_effects;
            CHECK_EQ(n.mask_index, 0);
            CHECK_EQ(n.inputs[0], 0);   // reads the layer source
        }
    CHECK_EQ(chain_effects, 1);
}

TEST(graph_mask_overlay_output) {
    doc::Document d = make_doc();
    doc::Mask mask = make_mask(d, doc::MaskType::Shape);
    d.masks.push_back(mask);
    // Not referenced by any effect — the overlay still builds and shows
    // it, as the VIEWPORT TAP (v5.4): the output stays the composite.
    RenderGraph g = gfx::compile_graph(d, mask.id);
    CHECK(g.valid);
    CHECK(g.preview >= 0);
    CHECK(g.nodes[static_cast<size_t>(g.preview)].kind ==
          GraphNode::Kind::MaskShape);
    CHECK(g.nodes[static_cast<size_t>(g.output)].kind !=
          GraphNode::Kind::MaskShape);
}

TEST(mask_bypassed_chain_effect_skipped) {
    doc::Document d = make_doc();
    doc::Mask mask = make_mask(d, doc::MaskType::Luma);
    mask.chain.push_back(doc::make_effect(d, doc::EffectType::Pixelate));
    mask.chain[0].bypass = true;
    d.masks.push_back(mask);
    d.layers[0].stack[0].mask_id = mask.id;

    RenderGraph g = gfx::compile_graph(d);
    CHECK(g.valid);
    for (const GraphNode& n : g.nodes)
        CHECK(!(n.kind == GraphNode::Kind::Effect && n.chain_index >= 0));
    // Extract reads the source directly.
    for (const GraphNode& n : g.nodes)
        if (n.kind == GraphNode::Kind::MaskExtract)
            CHECK_EQ(n.inputs[0], 0);
}
