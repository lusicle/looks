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
        Source,        // decoded frame -> linear RGB working target. Every
                       // clip source has its own, fed by the decode pool
                       // under this node's key - there is no shared
                       // playhead frame.
        Generator,     // solid/gradient/noise layer source (-1 = black)
        LayerTransform,// crop/flip/scale/rotate, pre-stack
        Effect,        // one EffectInstance dispatch
        Flow,          // block-match motion vectors (engine-fed planes)
        MatteExtract,  // port-1 matte: luma of the wired image
        MatteApply,    // inputs: dry, fx, matte
        LayerBlend,    // inputs: below, over. layer_index >= 0 reads the
                       // owning look layer's mode/opacity; -1 is a
                       // SEQUENCE lane stack: plain alpha-over, no modes.
    };
    Kind kind = Kind::Source;
    int layer_index = -1;      // owning layer (Effect/Generator/LayerBlend)
    int effect_index = -1;     // stack index within the layer
    int pass_index = 0;        // multi-pass effects (glow: bright/H/V/comp)
    std::vector<int> inputs;   // upstream node indices
    // Which instance owns this node: an index into
    // RenderGraph::instances. layer_index / effect_index address THAT
    // instance's look, never the root's.
    int instance = 0;
    // Instance-scoped identity of the node's subject: hash(instance
    // path, layer or effect id) - and for Source nodes the asset folds
    // in too, so a lane cutting between two looks never serves one
    // file's pixels under the other's key. Per-effect engine history and
    // per-clip decoded planes key on this. Razor-stable by construction:
    // paths fold container and target ids, never placement ids - with no
    // effects at sequence level there is nothing a cut could reset.
    uint64_t key = 0;
};

// One instance of an entity (look or sequence) inside the compiled
// graph. The root instance is index 0; every nested hop - a lane block
// or a lockstep ref source - spawns a child.
struct LookInstance {
    uint64_t look = 0;         // the entity these nodes read
    // Hash of the container chain from the root: the instance path.
    // Stable across frames, distinct per hop.
    uint64_t path = 0;
    // This instance's LOCAL time through the composed affine placement
    // maps. Composition stays CONTINUOUS all the way down - flooring at
    // every level would drift from the closed form the decode pool and the
    // audio mix evaluate. local_frame is the floor, and what effect
    // determinism, lanes and clocked effects run on.
    double local_time = 0.0;
    uint32_t local_frame = 0;
    int depth = 0;
};

struct RenderGraph {
    std::vector<GraphNode> nodes;
    std::vector<int> order;    // topological evaluation order
    // instances[0] is the compiled look itself; the rest are nested
    // placements, parents always before children.
    std::vector<LookInstance> instances;
    int output = -1;           // the REAL output — the composite, always
    // REFERENCE SOURCE: the first clip source playing in the root
    // instance, or -1. The A/B wipe compares against it and the shared
    // motion field is measured on it — a multi-clip look has no single
    // "the source", so the first one is named as the reference.
    int source = -1;
    // Viewport tap: the node the big preview publishes instead (selection
    // preview). -1 = show the output. Nothing else (thumbs, export,
    // out_source) ever reads it. Resolved in the root instance only —
    // you preview the look you are editing.
    int preview = -1;
    bool valid = false;        // false: cycle or empty
};

// Kahn topological sort. Returns false (and leaves `order` partial) when the
// edges contain a cycle.
bool topo_sort(const std::vector<GraphNode>& nodes, std::vector<int>& order);

// One entity -> graph, at one frame. root_id is a SEQUENCE (lanes
// resolve to their winning block and stack with plain alpha-over) or a
// LOOK (the scoped editor previews one directly). Bypassed effects are
// dropped at compile time; so are nested instances not playing at
// `frame`, which is what keeps a long arrangement from rendering
// everything every frame.
// preview_node publishes the named node's output instead of the
// composite — an effect/layer/group id, resolved when the root is a
// look; 0 (and anything unresolvable) keeps the composite.
// preview_layer publishes a whole contribution: at a look root the
// LAYER's chain end (pre-blend, pre-matte); at a sequence root the
// LANE's output (its winning block, pre-over) — the id names a layer or
// a lane track. preview_node outranks it. Preview-only: export passes 0
// for both.
RenderGraph compile_graph(const doc::Document& doc, uint64_t root_id,
                          uint32_t frame, uint64_t preview_node = 0,
                          uint64_t preview_layer = 0);

}  // namespace looks::gfx
