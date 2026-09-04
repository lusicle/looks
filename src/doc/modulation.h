// A wire replaces the base: final = clamp(min + span * curve(node), min, max).
// One param holds at most one wire, and the value graph stays acyclic.

#pragma once

#include <cstdint>
#include <vector>

namespace looks::doc {

// param_index >= 0 indexes params; negatives are built-ins (stack_commands.h).
// A layer key sets this bit and carries the layer id.
inline constexpr uint64_t kLayerParamBit = 1ull << 62;
// Slots 15, 16, 20 and 21 are field ids only, never modulatable.
inline constexpr int kLayerParamCount = 27;
// A group key sets this bit and carries the group id; wet/opacity only.
inline constexpr uint64_t kGroupParamBit = 1ull << 61;
// A gradient stop key sets this bit and carries the stop id.
// param_index: 0 position, 1 red, 2 green, 3 blue.
inline constexpr uint64_t kStopParamBit = 1ull << 60;

struct ParamKey {
    uint64_t effect_id = 0;
    int param_index = 0;

    bool operator==(const ParamKey& o) const {
        return effect_id == o.effect_id && param_index == o.param_index;
    }
};

enum class ModSourceType : uint32_t {
    Lfo = 0,
    Drift,
    AudioLow,
    AudioMid,
    AudioHigh,
    AudioOnset,
    VideoMotion,
    VideoBrightness,
    LfoBeat,
    Envelope,
    VideoCut,
    Beat,
    VideoSample,
    VideoRegion,
    Math,
    Normalise,
    // Camera: channel picks stab x/y/rot/scale or an anchor projection.
    Camera,
    Hold,
    Sequence,
    Count,
};

inline bool value_kind_is_triggered(ModSourceType t) {
    return t == ModSourceType::Hold || t == ModSourceType::Sequence;
}

// These kinds need audio_src wired. Unwired reads 0, never a global curve.
inline bool value_kind_wants_audio(ModSourceType t) {
    return t == ModSourceType::AudioLow || t == ModSourceType::AudioMid ||
           t == ModSourceType::AudioHigh || t == ModSourceType::AudioOnset ||
           t == ModSourceType::Beat || t == ModSourceType::LfoBeat ||
           t == ModSourceType::Envelope;
}

inline bool value_kind_wants_media(ModSourceType t) {
    return value_kind_wants_audio(t) || t == ModSourceType::Camera;
}

enum class LfoShape : uint32_t { Sine = 0, Triangle, Square, SampleHold, Count };

enum class ResponseCurve : uint32_t { Linear = 0, Exp, SCurve, Inverted, Count };

enum class ValueOp : uint32_t {
    Add = 0, Subtract, Multiply, Divide, Min, Max, Floor, Absolute, Count,
};

struct ModSource {
    ModSourceType type = ModSourceType::Lfo;
    LfoShape shape = LfoShape::Sine;
    float rate_hz = 1.0f;     // hz; LfoBeat reads it as beats per cycle
    float phase = 0.0f;       // cycles
    uint64_t seed = 0;
    // Keypress fires in live mode only; Beat reuses attack and decay.
    float attack = 0.02f;     // seconds to peak
    float decay = 0.4f;       // exponential decay constant, seconds
    uint32_t trigger = 0;     // 0 onset, 1 scene cut, 2 beat, 3 keypress
    // px/py/pw/ph are uv 0..1. VideoSample ignores the extent.
    // channel: 0 luma, 1 R, 2 G, 3 B.
    float px = 0.5f, py = 0.5f;
    float pw = 0.25f, ph = 0.25f;
    uint32_t channel = 0;
    // A solved feature-track id. 0 = none.
    uint32_t anchor = 0;
};

// An unwired helper input (0) reads its constant instead.
struct ValueNode {
    uint64_t id = 0;
    ModSource source;
    ValueOp op = ValueOp::Add;
    uint64_t in_a = 0, in_b = 0;    // upstream value-node ids; 0 = constant
    float const_a = 0.0f, const_b = 1.0f;
    // A layer or effect id in the same look. 0 = unwired = the node reads 0.
    uint64_t audio_src = 0;
    // Window [in_min, in_max], capped -1..1, scaled by m = const_b.
    float in_min = 0.0f, in_max = 1.0f;
    // Node-canvas position. (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
};

inline bool value_node_wants_media(const ValueNode& n) {
    if (value_kind_wants_media(n.source.type)) return true;
    return value_kind_is_triggered(n.source.type) &&
           (n.source.trigger == 1 || n.source.trigger == 2);
}

// node 0 = dangling: inert, kept so undo can restore its source.
struct ModRoute {
    uint64_t id = 0;
    uint64_t node = 0;
    ParamKey target;
    ResponseCurve curve = ResponseCurve::Linear;
};

// Handles are (dframe, dvalue) offsets: out_* to next, in_* from previous.
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
    bool loop = false;
    bool muted = false;
};

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
