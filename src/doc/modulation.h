// Modulation data model, stored in the Document: the look-local value
// graph (value nodes + routes), per-param keyframe lanes, and snapshot
// slots. Evaluation lives in src/mod/ — this header is pure data so the
// document stays self-contained.
//
// Value model per param:
//   unwired: the stored base (slider); keyframe lanes drive it
//   wired:   final = clamp(min + span · curve(node), min, max)
// A wire REPLACES the base — depth and offset shaping live in the value
// graph (math, normalise), never on the wire.
//
// A VALUE NODE is a shared signal: a generator (LFO, drift, audio bands,
// video sampling) or a helper that combines upstream node outputs (math
// ops, normalise). A ROUTE is a plain wire from one value node's output
// onto one param — one node can drive many params, a param holds at most
// ONE wire (adding to a wired param replaces it), and helper nodes chain
// node-to-node. The value graph is acyclic (commands guard wiring).
//
// Targets address effects by STABLE ID (survives reorder/undo); display
// paths like "layer0.fx2.shift_x" are derived from the live stack by the
// param table.

#pragma once

#include <cstdint>
#include <vector>

namespace looks::doc {

// param_index >= 0 indexes EffectInstance::params; negatives address the
// built-ins (kWetParam / kOpacityParam in stack_commands.h).
//
// Layer params are mod targets too: keys set kLayerParamBit and
// carry the LAYER id (layers share the effect id counter, but the bit
// keeps the addressing self-describing and JSON stores it as its own
// field). Indices — the continuous Layer fields:
//   0 opacity, 1-3 color_a rgb, 4-6 color_b rgb, 7 gen_scale,
//   8 gen_angle, 9 crop_l, 10 crop_r, 11 crop_t, 12 crop_b,
//   13 xf_scale, 14 xf_rotate.
inline constexpr uint64_t kLayerParamBit = 1ull << 62;
inline constexpr int kLayerParamCount = 15;

struct ParamKey {
    uint64_t effect_id = 0;
    int param_index = 0;

    bool operator==(const ParamKey& o) const {
        return effect_id == o.effect_id && param_index == o.param_index;
    }
};

enum class ModSourceType : uint32_t {
    Lfo = 0,
    Drift,             // Perlin-style wander
    AudioLow,          // analysis curves (import-time, sampled per frame)
    AudioMid,
    AudioHigh,
    AudioOnset,
    VideoMotion,
    VideoBrightness,
    LfoBeat,           // BPM-synced LFO: rate_hz = beats per cycle
    Envelope,          // attack-decay burst fired by triggers (see below)
    VideoCut,          // scene-cut trigger curve
    Beat,              // deterministic pulse train from the BPM estimate
    VideoSample,       // color/luma at a point of the current source frame
    VideoRegion,       // mean color/luma over a rect of the source frame
    Math,              // helper: op(a, b) over upstream nodes / constants
    Normalise,         // helper: map the scaled window onto [0, 1], clamped
    Count,
};

enum class LfoShape : uint32_t { Sine = 0, Triangle, Square, SampleHold, Count };

enum class ResponseCurve : uint32_t { Linear = 0, Exp, SCurve, Inverted, Count };

enum class ValueOp : uint32_t {
    Add = 0, Subtract, Multiply, Divide, Min, Max, Floor, Absolute, Count,
};

struct ModSource {
    ModSourceType type = ModSourceType::Lfo;
    LfoShape shape = LfoShape::Sine;
    float rate_hz = 1.0f;     // LFO / drift / S&H rate; LfoBeat: beats/cycle
    float phase = 0.0f;       // cycles
    uint64_t seed = 0;        // S&H / drift
    // Envelope: attack-decay burst fired by a trigger. Keypress
    // fires live only (exempts live mode from determinism); the
    // Beat source reuses attack/decay for its pulse shape.
    float attack = 0.02f;     // seconds to peak
    float decay = 0.4f;       // exponential decay constant, seconds
    uint32_t trigger = 0;     // 0 onset, 1 scene cut, 2 beat, 3 keypress
    // Video sampling (docs/flow_canvas.md): point / centered region on
    // the CURRENT decoded source frame, uv 0..1. VideoSample ignores the
    // extent (it averages a small fixed box so 8-bit code-value steps
    // don't pop). channel: 0 luma, 1 R, 2 G, 3 B.
    float px = 0.5f, py = 0.5f;
    float pw = 0.25f, ph = 0.25f;
    uint32_t channel = 0;
};

// A node of the value graph. source.type is the node kind; generator
// kinds read the ModSource fields, helper kinds read the fields below.
// An unwired helper input (0) reads its constant instead.
struct ValueNode {
    uint64_t id = 0;
    ModSource source;
    // Math: out = op(a, b).
    ValueOp op = ValueOp::Add;
    uint64_t in_a = 0, in_b = 0;    // upstream value-node ids; 0 = constant
    float const_a = 0.0f, const_b = 1.0f;
    // Normalise: the window [in_min, in_max] (bounds capped -1..1)
    // scaled by the multiplier m = const_b, so wide windows come from
    // the multiplier, not wide sliders:
    //   out = clamp01((a - in_min*m) / ((in_max - in_min)*m))
    float in_min = 0.0f, in_max = 1.0f;
    // Node-canvas position (docs/flow_canvas.md); (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
};

// A wire: one value node's output REPLACING one param (the node's 0..1
// maps onto the param's range through the curve). node 0 = dangling
// (inert, kept so undo can resurrect its source).
struct ModRoute {
    uint64_t id = 0;          // stable identity for UI/undo
    uint64_t node = 0;        // source ValueNode id
    ParamKey target;
    ResponseCurve curve = ResponseCurve::Linear;
};

// Cubic bezier key. Handles are (dframe, dvalue) offsets from the key —
// out_* eases toward the next key, in_* eases from the previous. hold
// steps to the next key with no interpolation.
struct Keyframe {
    double frame = 0.0;
    float value = 0.0f;
    float out_dx = 0.0f, out_dy = 0.0f;
    float in_dx = 0.0f, in_dy = 0.0f;
    bool hold = false;
};

struct KeyframeLane {
    ParamKey target;
    std::vector<Keyframe> keys;   // sorted by frame
    // Loopable region: once the playhead passes the first key,
    // evaluation wraps through the key span instead of holding the last
    // value — draw a cycle once, it repeats forever.
    bool loop = false;
    // Muted lanes keep their keys but stop driving the param (the
    // timeline's disable toggle).
    bool muted = false;
};

// Full parameter snapshot: stack param values keyed by effect id.
struct SnapshotEntry {
    uint64_t effect_id = 0;
    std::vector<float> params;
    float wet = 1.0f;
    float opacity = 1.0f;
};

struct Snapshot {
    bool valid = false;
    std::vector<SnapshotEntry> entries;
};

}  // namespace looks::doc
