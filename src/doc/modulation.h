// Modulation data model (spec §7), stored in the Document: the global mod
// matrix (routes), per-param keyframe lanes, and snapshot slots. Evaluation
// lives in src/mod/ — this header is pure data so the document stays
// self-contained.
//
// Value model per param:
//   final = clamp(base + Σ amount_i · curve_i(source_i))
// Keyframe lanes drive the BASE (keyframes set base, sources wiggle on top).
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
// Masks are mod targets too (spec §8): their keys set kMaskParamBit and
// carry the MASK id (mask ids come from a separate counter, so the bit
// keeps the two id spaces from colliding). Mask param indices: 0 feather,
// 1 center_x, 2 center_y, 3 radius_x, 4 radius_y, 5 blur_px,
// 6 black_point, 7 white_point, 8 gamma, 9 key_center, 10 key_range.
// Bezier shape points are addressable past kMaskPointParamBase (spec §8
// "keyframable points"): base + 2i = point i x, base + 2i + 1 = point i y.
inline constexpr uint64_t kMaskParamBit = 1ull << 63;
inline constexpr int kMaskPointParamBase = 16;

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
    LfoBeat,           // BPM-synced LFO: rate_hz = beats per cycle (spec §7)
    Envelope,          // attack-decay burst fired by triggers (see below)
    VideoCut,          // scene-cut trigger curve
    Beat,              // deterministic pulse train from the BPM estimate
    VideoSample,       // color/luma at a point of the current source frame
    VideoRegion,       // mean color/luma over a rect of the source frame
    Count,
};

enum class LfoShape : uint32_t { Sine = 0, Triangle, Square, SampleHold, Count };

enum class ResponseCurve : uint32_t { Linear = 0, Exp, SCurve, Inverted, Count };

struct ModSource {
    ModSourceType type = ModSourceType::Lfo;
    LfoShape shape = LfoShape::Sine;
    float rate_hz = 1.0f;     // LFO / drift / S&H rate; LfoBeat: beats/cycle
    float phase = 0.0f;       // cycles
    uint64_t seed = 0;        // S&H / drift
    // Envelope (spec §7): attack-decay burst fired by a trigger. Keypress
    // fires live only (spec §11 exempts live mode from determinism); the
    // Beat source reuses attack/decay for its pulse shape.
    float attack = 0.02f;     // seconds to peak
    float decay = 0.4f;       // exponential decay constant, seconds
    uint32_t trigger = 0;     // 0 onset, 1 scene cut, 2 beat, 3 keypress
    // Video sampling (docs/flow_canvas.md v4): point / centered region on
    // the CURRENT decoded source frame, uv 0..1. VideoSample ignores the
    // extent (it averages a small fixed box so 8-bit code-value steps
    // don't pop). channel: 0 luma, 1 R, 2 G, 3 B.
    float px = 0.5f, py = 0.5f;
    float pw = 0.25f, ph = 0.25f;
    uint32_t channel = 0;
};

struct ModRoute {
    uint64_t id = 0;          // stable identity for UI/undo
    ModSource source;
    ParamKey target;
    float amount = 0.0f;      // in normalized param range (-1..1 of span)
    ResponseCurve curve = ResponseCurve::Linear;
    // Node-canvas position (docs/flow_canvas.md); (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
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
    // Loopable region (spec §7): once the playhead passes the first key,
    // evaluation wraps through the key span instead of holding the last
    // value — draw a cycle once, it repeats forever.
    bool loop = false;
    // Muted lanes keep their keys but stop driving the param (the
    // timeline's disable toggle).
    bool muted = false;
};

// Full parameter snapshot (spec §7): stack param values keyed by effect id.
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
