// Project document (spec §5). Milestone-3 shape: one implicit layer holding
// one effect stack. Layers, assets, masks, and the mod matrix land in later
// milestones — but every mutation already goes through doc::Command so undo
// never has to be retrofitted.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doc/effect_instance.h"
#include "doc/masks.h"
#include "doc/modulation.h"

namespace looks::doc {

// Layer source (spec §5): the clip, a generator, or an adjustment layer
// whose stack applies to the composite below it.
enum class LayerSourceKind : uint32_t {
    Clip = 0,
    Solid,
    Gradient,
    Noise,
    TestPattern,   // 75% color bars + grayscale ramp (spec §5)
    Oscillator,    // video-synth periodic source: bars / rings / plasma
    Adjustment,
    Count,
};

// Groups (spec §5): a Group collapses a sub-stack and exposes macro knobs;
// a saved group IS an "era preset". Membership is a tag on the effect
// (EffectInstance::group_id) — groups don't change render order, only UI
// folding, shared bypass, and macro routing.
//
// A macro knob drives N targets: as the knob sweeps 0→1, each target param
// gets base + lerp(lo, hi, curve(value)) · param_span added on top — the
// same normalized-span semantics as a ModRoute amount, so lo=0 makes the
// knob neutral at rest. Targets live inside the group (not the global mod
// matrix) so a preset file is fully self-contained.
struct MacroTarget {
    uint64_t effect_id = 0;
    int param_index = 0;       // negatives = wet/opacity built-ins
    float lo = 0.0f;
    float hi = 0.5f;
    ResponseCurve curve = ResponseCurve::Linear;
};

struct MacroKnob {
    std::string name;
    float value = 0.0f;        // 0..1
    std::vector<MacroTarget> targets;
};

struct Group {
    uint64_t id = 0;
    std::string name;
    bool folded = false;
    bool bypass = false;
    std::vector<MacroKnob> macros;
};

struct Layer {
    uint64_t id = 0;
    std::string name;
    LayerSourceKind source = LayerSourceKind::Clip;
    // Generator params: color_a (solid / gradient start), color_b
    // (gradient end), scale (noise cell size), angle (gradient direction).
    float color_a[3] = {0.5f, 0.5f, 0.5f};
    float color_b[3] = {0.1f, 0.1f, 0.1f};
    float gen_scale = 6.0f;
    float gen_angle = 0.0f;
    // Oscillator waveform: 0 sine bars, 1 concentric rings, 2 plasma.
    uint32_t osc_shape = 0;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
    // Layer mask (spec §8: masks are referenced by effects AND layers):
    // gates this layer's whole contribution to the composite. 0 = none.
    uint64_t mask_id = 0;
    // Transform (spec §5: crop/flip/scale/rotate live on the layer, not in
    // stacks). Applied to the layer source BEFORE its stack — the rack
    // processes the transformed signal, so overlays/grids stay screen-
    // aligned. Above the bottom layer the blend gates to the transformed
    // region, so crop/scale-down reveal the composite below.
    float crop_l = 0.0f, crop_r = 0.0f;   // fraction of frame, 0..0.45
    float crop_t = 0.0f, crop_b = 0.0f;
    bool flip_h = false, flip_v = false;
    float xf_scale = 1.0f;                // about frame center, 0.25..4
    float xf_rotate = 0.0f;               // degrees, -180..180
    // Trim (spec §5): the segment of the source clip this layer plays,
    // frames [in, out). out 0 = clip end. The layer holds its last trimmed
    // frame past the segment (never blanks a looping preview).
    uint32_t trim_in = 0;
    uint32_t trim_out = 0;
    std::vector<EffectInstance> stack;
    std::vector<Group> groups;
};

inline bool layer_has_transform(const Layer& l) {
    return l.crop_l > 0.0f || l.crop_r > 0.0f || l.crop_t > 0.0f ||
           l.crop_b > 0.0f || l.flip_h || l.flip_v ||
           l.xf_scale != 1.0f || l.xf_rotate != 0.0f;
}

inline bool layer_has_trim(const Layer& l) {
    return l.source == LayerSourceKind::Clip &&
           (l.trim_in > 0 || l.trim_out > 0);
}

inline constexpr size_t kMaxLayers = 3;   // spec §5: start with 3 max
inline constexpr float kMaxSpeed = 4.0f;  // time-remap speed range 0..4

struct Document {
    std::string name = "untitled";
    // Source clip for the base layer (empty = test pattern). The asset list
    // proper arrives with multi-asset support; v1 is one clip per project.
    std::string clip_path;
    uint64_t master_seed = 0;
    // Frame render cache RAM budget in MiB (spec §10: default 2 GB,
    // configurable); 0 disables caching for this project.
    uint32_t cache_mb = 2048;
    // Half-res proxy toggle (spec §3/§10): preview decodes <stem>.proxy.mez
    // when present. Export always renders full-res.
    bool use_proxy = false;
    // Bumped on every executed/undone/redone command. Cheap "did anything
    // change" signal for autosave and dirty-flag UI; render caching keys on
    // per-subgraph hashes later, not on this.
    uint64_t revision = 0;

    // Bottom-up layer list (layers[0] is the base). Every document starts
    // with one clip layer.
    std::vector<Layer> layers;
    // Monotonic id source for stable identity (effects, layers, masks)
    // across reorder/undo.
    uint64_t next_effect_id = 1;

    Document() {
        Layer base;
        base.id = next_effect_id++;
        base.name = "layer 1";
        layers.push_back(std::move(base));
    }

    // Modulation (spec §7): global mod matrix + per-param keyframe lanes +
    // A/B/C snapshot slots.
    std::vector<ModRoute> mod_routes;
    std::vector<KeyframeLane> lanes;
    uint64_t next_route_id = 1;
    Snapshot snapshots[3];

    // Morph (spec §7): timed interpolation between two snapshot slots; the
    // position is itself a mod target (ParamKey {0, 0} = "global.morph").
    // Active only when both slots hold valid snapshots.
    int morph_from = 0;
    int morph_to = 1;
    float morph_pos = 0.0f;

    // Time remap (spec §6.1: speed ramp / reverse / ping-pong). Speed is a
    // mod target (ParamKey {0, 1} = "global.speed") so lanes ramp it; the
    // playback position is the prefix sum of per-frame speed (mod/eval.h).
    float speed = 1.0f;
    uint32_t time_mode = 0;   // 0 forward, 1 reverse, 2 ping-pong

    // Timeline region (spec §3/§9): clip trim [in, out) — out 0 = clip end
    // — edited by the ruler's trim handles; export renders the trim. The
    // loop region (0/0 = off) confines looping playback inside the trim.
    uint32_t clip_trim_in = 0;
    uint32_t clip_trim_out = 0;
    uint32_t loop_in = 0;
    uint32_t loop_out = 0;
    // Still-image clip length in frames (0 = as imported). The bundle on
    // the scratch cache is regenerable, so the one user decision baked
    // into it must live here; re-applied whenever the clip opens.
    uint32_t still_duration_frames = 0;

    // Sidechain (spec §7): analyze an external WAV or another MP4's audio
    // instead of the clip's own; the audio-derived mod curves come from it
    // while video curves stay with the clip. sidechain_mux muxes its audio
    // into the export instead of the clip's. audio_offset_ms nudges audio
    // against video everywhere (curve sampling, monitoring, export).
    std::string sidechain_path;
    bool sidechain_mux = false;
    float audio_offset_ms = 0.0f;

    // Masks (spec §8): first-class named objects referenced by effects.
    std::vector<Mask> masks;
    uint64_t next_mask_id = 1;
};

}  // namespace looks::doc
