// Instance flatten: the nesting tree composed down to its LEAF MEDIA
// SOURCES in ROOT-LOCAL frames.
//
// Sequence placement is affine and look sources are lockstep (identity
// plus slip), so an entire arrangement - however deep, whichever way
// looks and sequences nest - reduces to a list of (span, source_in,
// speed) triples that is a pure function of the document. The decode
// pool walks this list instead of the tree, and the audio mix needs it:
// the callback runs ahead of the render and must know what plays at a
// sample position no frame walk has visited yet.
//
// The keys here are the SAME instance-scoped keys compile stamps on its
// Source nodes, which is what lets the pool decode for a graph it never
// sees. That agreement is a test, not a hope.

#pragma once

#include <cstdint>
#include <vector>

#include "doc/document.h"
#include "util/hash.h"

namespace looks::doc {

// Decode-stream identity of one media read: instance path + node +
// asset, with a non-zero Offset-node shift folded in (a shifted read is
// its own stream). TIMELINE-LOCKED nodes drop the path - every
// instance reads the asset at the root clock, so they all share one
// stream. compile_graph stamps Source nodes with EXACTLY this; the
// flatten emits the same - one formula so the two can't drift.
inline uint64_t media_stream_key(uint64_t path, uint64_t layer_id,
                                 uint64_t asset, bool locked,
                                 int64_t offset) {
    constexpr uint64_t kLockSalt = 0x74696d656c6f636bull;
    uint64_t k = hash_combine(
        hash_combine(locked ? kLockSalt : path, layer_id), asset);
    if (offset) k = hash_combine(k, static_cast<uint64_t>(offset));
    return k;
}

// Instance path of a nested ref's child, with a non-zero Offset-node
// shift folded in so a shifted hop keys its own effect history.
inline uint64_t nested_child_path(uint64_t path, uint64_t layer_id,
                                  uint64_t target, int64_t offset) {
    uint64_t k = hash_combine(hash_combine(path, layer_id), target);
    if (offset) k = hash_combine(k, static_cast<uint64_t>(offset));
    return k;
}

// A window whose end is the parent's end rather than its own.
inline constexpr double kUnbounded = 1e18;

// One audio-modifier hop on a voice: the effect's params snapshotted at
// flatten time (audio DSP params are not modulatable - the mix rebuilds
// on document revision, so slider edits land, value wires do not).
// Applied in vector order, source first.
struct AudioOp {
    EffectType type = EffectType::AudioGain;
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wet = 1.0f;
};

// Voice chains cap here; hops past the cap are dropped deterministically.
inline constexpr size_t kMaxVoiceOps = 8;

struct MediaInstance {
    uint64_t key = 0;    // hash(path, container id, asset) = GraphNode::key
    uint64_t owner = 0;  // the look or sequence holding the container
    uint64_t layer = 0;  // the layer (looks) or track (sequences) id
    uint64_t asset = 0;
    // source = (root - t_in) * speed + source_in, live on [t_in, t_out).
    double t_in = 0.0;
    double t_out = 0.0;
    double source_in = 0.0;
    double speed = 1.0;
    // Composed audio gain: the placement's, scaled by every enclosing
    // track and block, and 0 when anything on the path is muted.
    float gain = 1.0f;
    // AUDIO WALK ONLY: the voice's DSP chain, inner (nested) ops first,
    // outer ops appended; hops past the cap drop deterministically. A
    // fixed array keeps the instance trivially destructible (UI arenas
    // hold instances by value). The picture walk leaves it empty.
    AudioOp ops[kMaxVoiceOps];
    uint32_t op_count = 0;
};

// Every media source a picture walk from `root_id` (a look or a sequence)
// reaches, culled by visibility and nesting depth exactly as compile
// culls, in the same emission order. Two blocks of one entity appear
// twice, with different keys. This is the decode pool's walk.
std::vector<MediaInstance> flatten_media_sources(const Document& doc,
                                               uint64_t root_id);

// The AUDIO walk: sequences contribute their AUDIO TRACKS' placements
// (video lanes are silent - sound rides audio placements only), and a
// look contributes its VOICE: the chain wired into its Output (combined
// = the In wire, where the first port-0 link is the bottom chain and
// wins the fan-in; split = the dedicated audio-in, silent when
// unwired), audio-modifier hops collected into `ops` in play order.
// Video effects pass audio through; multi-input nodes carry port 0's
// audio; generators are silent; nested entities recurse, outer ops
// appending after inner. Gain composes as track gain * placement gain *
// everything enclosing; any mute on the path is 0.
std::vector<MediaInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id);

// The audio chain FEEDING a graph node, composed to closed form: walk
// back from `node` (a layer or effect id in `look`) through port-0
// inputs, collecting audio-modifier hops exactly as the voice walk
// does; nested look refs resolve through their own Output (inner ops
// first, the leaf's slip carried out). asset 0 = nothing resolvable
// behind the wire (generator, unwired, dangling, or a sequence ref -
// a sequence has no single voice). Runtime analysis keys on this.
struct AudioChain {
    uint64_t asset = 0;
    uint32_t slip = 0;
    // Summed Offset-node shift along the chain (audio-targeting nodes
    // sitting directly on their source, every nesting hop): the media
    // frame behind local L is L + slip + offset.
    int64_t offset = 0;
    // True when the chain's media node is timeline-locked (reads the
    // root clock, not the look's).
    bool locked = false;
    AudioOp ops[kMaxVoiceOps];
    uint32_t op_count = 0;
};
AudioChain resolve_audio_chain(const Document& doc, const Look& look,
                               uint64_t node);

inline bool media_active(const MediaInstance& c, double root_frame) {
    return root_frame >= c.t_in && root_frame < c.t_out;
}

// The asset frame this instance shows at a root frame. Callers clamp into
// the asset - a window may outlive its media.
inline double media_source_frame(const MediaInstance& c, double root_frame) {
    return (root_frame - c.t_in) * c.speed + c.source_in;
}

}  // namespace looks::doc
