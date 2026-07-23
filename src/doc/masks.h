// Mask objects (spec §8) — first-class, named, referenced by any number of
// effects. v1 types: procedural shape (ellipse/rect), luma-from-source
// (with a mini effect chain evaluated before extraction — the reason the
// renderer is a DAG), and luma/chroma keys. Motion masks join with the flow
// module (frame differencing here would break scrub determinism).
//
// Extraction pipeline per mask: source → [mini fx chain] → extract
// (luma | channel | inverted) → levels (black/white/gamma) → feather blur.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doc/effect_instance.h"

namespace looks::doc {

enum class MaskType : uint32_t {
    Shape = 0,     // procedural ellipse/rect, analytic feather
    Luma,          // grayscale from the layer source (mini chain first)
    LumaKey,       // keyed range of luma
    ChromaKey,     // keyed distance from a reference color
    Motion,        // optical-flow magnitude (levels = the threshold)
    Count,
};

enum class MaskExtract : uint32_t {
    Luma = 0,
    Red,
    Green,
    Blue,
    Alpha,
    Count,
};

// Channel combine ops (spec §8): fold another mask in after levels.
enum class MaskCombineOp : uint32_t {
    Add = 0,
    Subtract,
    Intersect,
    Count,
};

struct Mask {
    uint64_t id = 0;
    std::string name;
    MaskType type = MaskType::Shape;

    // Shape (normalized 0..1 coordinates; feather in normalized units).
    float center_x = 0.5f, center_y = 0.5f;
    float radius_x = 0.3f, radius_y = 0.3f;
    float roundness = 1.0f;    // 1 = ellipse, 0 = rectangle
    float feather = 0.05f;
    // Bezier shape (spec §8): >= 3 normalized (x, y) pairs switch the
    // shape from the procedural ellipse/rect to a smooth closed path
    // through the points (Catmull-Rom, flattened engine-side).
    std::vector<float> points;

    // Luma / keys.
    MaskExtract extract = MaskExtract::Luma;
    float key_center = 0.5f;   // luma key center / chroma reference via RGB
    float key_range = 0.25f;
    float key_r = 0.0f, key_g = 1.0f, key_b = 0.0f;
    float blur_px = 0.0f;      // feather blur for extracted masks

    // Levels (users will live in these — spec §8).
    float black_point = 0.0f;
    float white_point = 1.0f;
    float gamma = 1.0f;
    bool invert = false;

    // Mask source (spec §8: "source can be anything"). Priority:
    // generator > another layer > external video > the shared clip source.
    // source_layer_id names a layer whose SOURCE (pre-stack: its clip trim
    // or its generator) feeds the mask — post-stack would allow cycles.
    // source_gen is a LayerSourceKind generator value (Noise/TestPattern/
    // Gradient) rendered white-on-black with the mask's own scale/angle.
    std::string source_path;   // .mez bundle
    uint64_t source_layer_id = 0;
    uint32_t source_gen = 0;   // 0 = none, else LayerSourceKind
    float gen_scale = 24.0f;   // generator cell size (px)
    float gen_angle = 0.0f;    // gradient angle (radians)
    bool free_run = false;
    uint32_t fit = 0;          // 0 stretch, 1 fill, 2 tile

    // Post (spec §8): expand (+) / contract (-) in pixels, applied before
    // the feather blur.
    float grow_px = 0.0f;

    // Combine (spec §8): fold another mask in after this one's levels.
    uint64_t combine_id = 0;   // 0 = none; self/cycles are ignored
    MaskCombineOp combine_op = MaskCombineOp::Add;

    // Mini effect chain applied to the mask source before extraction.
    std::vector<EffectInstance> chain;
};

}  // namespace looks::doc
