// Keep this TU Vulkan-free; unit tests link it without a GPU.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "doc/document.h"

namespace looks::gfx {

// A source card taps on its layer id with this bit; other cards use a bare id.
inline constexpr uint64_t kThumbSourceBit = 1ull << 62;

struct GraphNode {
    enum class Kind : uint8_t {
        Source,        // decode pool feeds it under this node's key
        Generator,     // layer source; layer_index -1 = premultiplied zero
        LayerTransform,// pre-stack crop/flip/scale/rotate
        Effect,
        Flow,          // motion vectors; the engine feeds the planes
        MatteExtract,  // port-1 matte: luma of the wired image
        MatteApply,    // inputs: dry, fx, matte
        LayerBlend,    // inputs: below, over
                       // layer_index >= 0 = look layer; -1 = sequence lane
        GroupMix,      // inputs: dry, face
                       // wet/opacity: layers[layer_index].groups[effect_index]
        Crossfade,
    };
    Kind kind = Kind::Source;
    int layer_index = -1;
    int effect_index = -1;     // stack index within the layer
    int pass_index = 0;
    std::vector<int> inputs;   // upstream node indices
    // instances index; layer/effect indices address that instance's look.
    int instance = 0;
    // hash(instance path, layer or effect id); Source folds the asset in.
    // Engine history and decoded planes key on this; never fold placement ids.
    uint64_t key = 0;
    // Sequence lanes only: the lane's LayerBlend applies this at composite.
    float p_shift_x = 0.0f;
    float p_shift_y = 0.0f;
    float p_scale = 1.0f;
    float p_rotate = 0.0f;   // radians, pre-converted at compile
    float p_opacity = 1.0f;
    // pivot in canvas fractions
    float p_anchor_x = 0.5f;
    float p_anchor_y = 0.5f;
};

struct LookInstance {
    uint64_t look = 0;
    // hash of the container chain from the root; stable across frames
    uint64_t path = 0;
    // Local time stays continuous down the nest; floor only at the end.
    // local_frame is that floor; clocked effects run on it.
    double local_time = 0.0;
    uint32_t local_frame = 0;
    int depth = 0;
};

struct RenderGraph {
    std::vector<GraphNode> nodes;
    std::vector<int> order;
    // instances[0] is the root; parents always come before children.
    std::vector<LookInstance> instances;
    int output = -1;           // the composite, always
    // First media source the compile emits, or -1.
    int source = -1;
    // Viewport preview tap, root instance only; -1 = show the output.
    int preview = -1;
    // Card thumbnails, root instance only: card id to the node that makes
    // that card's output. Only the compiler knows where a card really ends.
    std::vector<std::pair<uint64_t, int>> thumb_taps;
    // Selected block's lane image before its Motion, root sequence only.
    // UI-only: rendered pixels never depend on the measurement.
    int measure = -1;
    // The composite with every effect stack stripped; -1 unless with_before.
    int before = -1;
    bool valid = false;        // false: cycle or empty
};

// False on a cycle; order is left partial.
bool topo_sort(const std::vector<GraphNode>& nodes, std::vector<int>& order);

// root_id is a sequence or a look; preview_node outranks preview_layer.
// Bypassed effects and instances not playing at frame drop at compile time.
RenderGraph compile_graph(const doc::Document& doc, uint64_t root_id,
                          uint32_t frame, uint64_t preview_node = 0,
                          uint64_t preview_layer = 0,
                          uint64_t measure_placement = 0,
                          bool with_before = false);

// All target sizing must go through this function; a mismatch drops frames.
inline uint32_t even_down(uint32_t v, uint32_t div) {
    const uint32_t d = (v / (div ? div : 1)) & ~1u;
    return d > 2u ? d : 2u;
}

// rect = {x, y, w, h} in output pixels, centered, aspect preserved.
// Matching aspects return exactly the full target; unknown dims fill.
inline void source_fit_rect(float src_w, float src_h, float out_w,
                            float out_h, float rect[4], uint32_t mode = 0) {
    float fw = out_w;
    float fh = out_h;
    if (mode != 2 && src_w > 0.0f && src_h > 0.0f && out_w > 0.0f && out_h > 0.0f) {
        const float sa = src_w / src_h;
        const float oa = fw / fh;
        if ((sa > oa) != (mode == 1))
            fh = fw / sa;
        else
            fw = fh * sa;
    }
    rect[0] = (out_w - fw) * 0.5f;
    rect[1] = (out_h - fh) * 0.5f;
    rect[2] = fw;
    rect[3] = fh;
}

}  // namespace looks::gfx
