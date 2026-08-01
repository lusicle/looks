// Render graph: the document compiles to a DAG of passes over pooled
// targets; the evaluator walks a topological order. Wires branch freely; a
// matted effect forms a diamond
//   upstream ──┬── fx ─────────────┐
//              │                   ├─ matte_apply
//              └── matte_extract ──┘
// where the extract's input is whatever image the port-1 wire carries.
// Cycles are illegal except through the explicit Feedback node (which
// reads the previous frame, so it never forms an edge here).
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
        Generator,     // solid/gradient/noise layer source (-1 = black)
        LayerTransform,// crop/flip/scale/rotate, pre-stack
        Effect,        // one EffectInstance dispatch
        Flow,          // block-match motion vectors (engine-fed planes)
        MatteExtract,  // port-1 matte: luma of the wired image
        MatteApply,    // inputs: dry, fx, matte
        LayerBlend,    // inputs: below, layer output
    };
    Kind kind = Kind::Source;
    int layer_index = -1;      // owning layer (Effect/Generator/LayerBlend)
    int effect_index = -1;     // stack index within the layer
    int pass_index = 0;        // multi-pass effects (glow: bright/H/V/comp)
    std::vector<int> inputs;   // upstream node indices
};

struct RenderGraph {
    std::vector<GraphNode> nodes;
    std::vector<int> order;    // topological evaluation order
    int output = -1;           // the REAL output — the composite, always
    // Viewport tap: the node the big preview publishes instead (selection
    // preview). -1 = show the output. Nothing else (thumbs, export,
    // out_source) ever reads it.
    int preview = -1;
    bool valid = false;        // false: cycle or empty
};

// Kahn topological sort. Returns false (and leaves `order` partial) when the
// edges contain a cycle.
bool topo_sort(const std::vector<GraphNode>& nodes, std::vector<int>& order);

// Document -> graph. Bypassed effects are dropped at compile time.
// preview_node publishes the named node's output instead of the composite
// — an effect/layer/group id; 0 (and anything unresolvable) keeps the
// composite. Preview-only: export passes 0.
RenderGraph compile_graph(const doc::Document& doc,
                          uint64_t preview_node = 0);

}  // namespace looks::gfx
