// Render graph (spec §4): the document compiles to a DAG of passes over
// pooled targets; the evaluator walks a topological order. Stacks are
// linear chains; masks add real branches — a masked effect forms a diamond
//   upstream ──┬── fx ──────────┐
//              │                ├─ mask_apply
//              └── mask subgraph┘
// where the mask subgraph is source → [mini fx chain] → extract → blur
// (spec §8: every mask source owns an optional effect chain). Cycles are
// illegal except through the explicit Feedback node (which reads the
// previous frame, so it never forms an edge here).
//
// No Vulkan in this TU — the compiler/evaluator core is unit-tested CPU-side.

#pragma once

#include <cstdint>
#include <vector>

#include "doc/document.h"

namespace looks::gfx {

struct GraphNode {
    enum class Kind : uint8_t {
        Source,        // decoded frame -> linear RGB working target.
                       // layer_index >= 0: a private per-layer source (the
                       // layer's trim selects a different clip frame,
                       // engine-fed); -1: the shared playhead frame.
        Generator,     // solid/gradient/noise layer source
        LayerTransform,// crop/flip/scale/rotate (spec §5), pre-stack
        Effect,        // one EffectInstance dispatch (stack or mask chain)
        Flow,          // block-match motion vectors (engine-fed planes)
        MaskShape,     // procedural shape -> mask target (no inputs)
        MaskSource,    // external mask-source video frame (engine-fed)
        MaskExtract,   // grayscale + levels (+ key modes; motion = flow)
        MaskMorphH,    // separable expand/contract (spec §8 post)
        MaskMorphV,
        MaskBlurH,     // separable feather
        MaskBlurV,
        MaskCombine,   // inputs: this mask, other mask (add/sub/intersect)
        MaskApply,     // inputs: dry, fx, mask
        LayerBlend,    // inputs: below, layer output
    };
    Kind kind = Kind::Source;
    int layer_index = -1;      // owning layer (Effect/Generator/LayerBlend)
    int effect_index = -1;     // stack index within the layer
    int mask_index = -1;       // doc.masks index (mask nodes + chain effects)
    int chain_index = -1;      // position in the mask's mini chain
    int pass_index = 0;        // multi-pass effects (glow: bright/H/V/comp)
    std::vector<int> inputs;   // upstream node indices
};

struct RenderGraph {
    std::vector<GraphNode> nodes;
    std::vector<int> order;    // topological evaluation order
    int output = -1;           // node whose result reaches the viewport
    bool valid = false;        // false: cycle or empty
};

// Kahn topological sort. Returns false (and leaves `order` partial) when the
// edges contain a cycle.
bool topo_sort(const std::vector<GraphNode>& nodes, std::vector<int>& order);

// Document -> graph. Bypassed effects are dropped at compile time; shared
// masks compile once and fan out. If overlay_mask_id names an existing
// mask, the graph output becomes that mask's grayscale (viewport overlay
// visualization, spec §8).
RenderGraph compile_graph(const doc::Document& doc,
                          uint64_t overlay_mask_id = 0);

}  // namespace looks::gfx
