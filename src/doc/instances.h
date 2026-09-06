// The flatten is a pure function of the document, in root-local frames.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "doc/document.h"
#include "util/hash.h"

namespace looks::doc {

// compile_graph must stamp Source nodes with exactly this key.
// A locked node drops the path, so every instance shares one stream.
inline uint64_t media_stream_key(uint64_t path, uint64_t layer_id,
                                 uint64_t asset, bool locked,
                                 int64_t offset) {
    constexpr uint64_t kLockSalt = 0x74696d656c6f636bull;
    uint64_t k = hash_combine(
        hash_combine(locked ? kLockSalt : path, layer_id), asset);
    if (offset) k = hash_combine(k, static_cast<uint64_t>(offset));
    return k;
}

// A non-zero Offset shift folds in: a shifted hop keys its own history.
inline uint64_t nested_child_path(uint64_t path, uint64_t layer_id,
                                  uint64_t target, int64_t offset) {
    uint64_t k = hash_combine(hash_combine(path, layer_id), target);
    if (offset) k = hash_combine(k, static_cast<uint64_t>(offset));
    return k;
}

// The lane and the target fold in. compile_graph stamps exactly this too.
inline uint64_t seq_child_path(uint64_t path, uint64_t container,
                               uint64_t target) {
    return hash_combine(hash_combine(path, container), target);
}

// End sentinel: the window runs to the parent end.
inline constexpr double kUnbounded = 1e18;

// rate is media frames per local frame; length 0 = unbounded.
// The flatten and the compiler share this one formula: lo <= t < hi.
inline void shifted_window(double length, int64_t shift, double rate,
                           double* lo, double* hi) {
    const double r = rate > 0.0 ? rate : 1.0;
    *lo = shift < 0 ? static_cast<double>(-shift) / r : 0.0;
    *hi = kUnbounded;
    if (length > 0.0) {
        *hi = (length - static_cast<double>(shift)) / r;
        if (*hi < *lo) *hi = *lo;
    }
}

// Params snapshot at flatten time: audio DSP params are not modulatable.
struct AudioOp {
    EffectType type = EffectType::AudioGain;
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wet = 1.0f;
};

// Chains cap here. Hops past the cap drop deterministically.
inline constexpr size_t kMaxVoiceOps = 8;

struct AudioNode {
    // Leaf read; 0 = interior node.
    uint64_t asset = 0;
    uint64_t key = 0;     // media_stream_key, matching the compiler
    uint64_t owner = 0;
    uint64_t layer = 0;
    // Effect id on op nodes, layer id on leaves, 0 on sums and hops.
    uint64_t doc_id = 0;
    // Composed clock: local = a * root_frame + b, ticking at local_fps.
    double a = 1.0, b = 0.0;
    double local_fps = 30.0;
    // Media frames per local frame; shift is media-frame exact.
    double rate = 1.0;
    int64_t shift = 0;
    // Root-frame span. Leaf spans are for the leaf view only.
    bool windowed = false;
    double w0 = 0.0, w1 = kUnbounded;
    float gain = 1.0f;
    bool has_op = false;
    AudioOp op;
    std::vector<int> inputs;
};

// The node count caps deterministically; branches past the cap drop.
inline constexpr size_t kMaxAudioNodes = 1024;

struct AudioProgram {
    std::vector<AudioNode> nodes;
    int root = -1;   // -1 = silent
};

// A fan-in sums every live feed; the Feedback back edge stays silent.
AudioProgram flatten_audio_program(const Document& doc, uint64_t root_id);

struct MediaInstance {
    uint64_t key = 0;    // hash(path, container id, asset) = GraphNode::key
    uint64_t owner = 0;
    uint64_t layer = 0;  // layer id in looks, track id in sequences
    uint64_t asset = 0;
    // source = (root - t_in) * speed + source_in, live on [t_in, t_out).
    double t_in = 0.0;
    double t_out = 0.0;
    double source_in = 0.0;
    double speed = 1.0;
    // Media frames per composed clock frame.
    double rate = 1.0;
    // Media-frame exact: applied after the rate, never before the floor.
    int64_t shift = 0;
    // Composed gain: 0 when anything on the path is muted.
    float gain = 1.0f;
    uint32_t slide_count = 0;
    uint32_t slide_index = 0;
    double slide_period = 0.0;
    double slide_fade = 0.0;
    uint32_t slide_end = 0;
    uint32_t repeat_frames = 0;
};

// Culls and emits in the same order as compile does.
std::vector<MediaInstance> flatten_media_sources(const Document& doc,
                                               uint64_t root_id);

// A leaf that a diamond reaches twice emits once.
std::vector<MediaInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id);

// True when something drawable reaches the Output's video port. A look
// with a split audio-only wiring reports false, like a silent look does.
bool entity_has_image(const Document& doc, uint64_t root_id);

// Keeps the bottom-most resolvable path: analysis needs one stream.
// asset 0 = nothing resolvable behind the wire.
struct AudioChain {
    uint64_t asset = 0;
    uint32_t slip = 0;
    // The media frame behind local L is floor(L * rate) + slip + offset.
    int64_t offset = 0;
    // Media frames per clock frame.
    double rate = 1.0;
    // The media node reads the root clock, not the look clock.
    bool locked = false;
    AudioOp ops[kMaxVoiceOps];
    uint32_t op_count = 0;
};
AudioChain resolve_audio_chain(const Document& doc, const Look& look,
                               uint64_t node);

inline bool media_active(const MediaInstance& c, double root_frame) {
    if (root_frame < c.t_in || root_frame >= c.t_out) return false;
    if (!c.slide_count) return true;
    const auto sample = slideshow_sample((root_frame - c.t_in) * c.speed + c.source_in,
        c.slide_period, c.slide_count, c.slide_fade, c.slide_end);
    return sample.current == c.slide_index || sample.previous == c.slide_index;
}

// The local-clock position only: no rate, no shift.
inline double media_source_frame(const MediaInstance& c, double root_frame) {
    return (root_frame - c.t_in) * c.speed + c.source_in;
}

// Callers must clamp into the asset: a window can outlive its media.
inline double media_asset_frame(const MediaInstance& c, double root_frame) {
    double source = media_source_frame(c, root_frame);
    if (c.slide_count) {
        const auto sample = slideshow_sample(source, c.slide_period,
            c.slide_count, c.slide_fade, c.slide_end);
        source = sample.current == c.slide_index ? sample.frame :
            std::max(0.0, c.slide_period - 1.0);
    }
    const double frame = std::floor(source * c.rate) + static_cast<double>(c.shift);
    return c.repeat_frames ? std::fmod(std::max(0.0, frame), c.repeat_frames) : frame;
}

}  // namespace looks::doc
