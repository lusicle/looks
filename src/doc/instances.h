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

#include <cmath>
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

// Instance path of a sequence placement's child: the lane AND the
// target fold in, so a razor moves nothing and two targets cut on one
// lane stay distinct. The flatten and compile_graph both stamp EXACTLY
// this - divergence would hand the decode pool keys the graph's Source
// nodes do not carry.
inline uint64_t seq_child_path(uint64_t path, uint64_t container,
                               uint64_t target) {
    return hash_combine(hash_combine(path, container), target);
}

// A window whose end is the parent's end rather than its own.
inline constexpr double kUnbounded = 1e18;

// Playable window of a shifted stream in LOCAL time: the media frame
// floor(local * rate) + shift must stay inside [0, length); length 0 =
// unbounded (hi = kUnbounded). rate is the media-hop conform ratio
// (media frames per local frame; 1 at every non-media hop). A negative
// shift delays the start (closed gate before it), a positive one
// shortens the tail. The flatten's intervals and the compiler's point
// tests are this ONE formula - live means lo <= t < hi.
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

// One audio-modifier hop: the effect's params snapshotted at flatten
// time (audio DSP params are not modulatable - the mix rebuilds on
// document revision, so slider edits land, value wires do not).
struct AudioOp {
    EffectType type = EffectType::AudioGain;
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wet = 1.0f;
};

// Analysis chains cap here; hops past the cap drop deterministically.
inline constexpr size_t kMaxVoiceOps = 8;

// One node of a look's AUDIO PROGRAM: the wired graph flattened to a
// DAG the mix evaluates directly. Leaves read media through composed
// affine clocks; interior nodes SUM their fan-in (combine-all - the
// same connection rule the video composite merges by); an op node
// applies its DSP to the summed signal on the owning look's clock.
// Sequence placements and nested duration cuts are WINDOWED hop nodes
// sitting ABOVE the DSP, so a razor is structural for sound too.
struct AudioNode {
    // Leaf read; 0 = interior node.
    uint64_t asset = 0;
    uint64_t key = 0;     // media_stream_key, matching the compiler
    uint64_t owner = 0;   // the look holding the media node
    uint64_t layer = 0;
    // The doc node behind this program node: the effect id on op
    // nodes, the layer id on leaves, 0 on synthetic sums/hops. Card
    // previews find their taps through this.
    uint64_t doc_id = 0;
    // Composed clock: local = a * root_frame + b, local ticking at
    // local_fps (the owning entity's effective rate).
    double a = 1.0, b = 0.0;
    double local_fps = 30.0;
    // Leaf conform: MEDIA frames per local frame, and the
    // media-frame-exact in-point (slip + Offset shims).
    double rate = 1.0;
    int64_t shift = 0;
    // Root-frame span. Hop nodes are windowed (contribution cut with
    // an edge ramp); leaves carry their playable span for the LEAF
    // VIEW only (the mix lets PCM bounds taper reads).
    bool windowed = false;
    double w0 = 0.0, w1 = kUnbounded;
    // Track/placement gain composed onto the hop it rides.
    float gain = 1.0f;
    bool has_op = false;
    AudioOp op;
    std::vector<int> inputs;
};

// The program's node count caps deterministically; branches past the
// cap drop bottom-kept.
inline constexpr size_t kMaxAudioNodes = 1024;

struct AudioProgram {
    std::vector<AudioNode> nodes;
    int root = -1;   // -1 = silent
};

// The AUDIO walk: the whole scoped entity's sound as ONE program.
// Sequences contribute their AUDIO TRACKS' placements as windowed hops
// (video lanes are silent - sound rides audio placements only); a look
// contributes every chain wired into its Output, SUMMED (combined =
// the port-0 In fan-in; split = the dedicated audio-in, silent when
// unwired). Connection-following is ONE rule for image and sound - a
// fan-in walks ALL live feeds, bottom-first - and only the mux
// differs: video stacks by layer order, audio sums at the node, so
// every op processes exactly the signal wired into it. Video effects
// pass audio through; multi-input nodes carry port 0's audio;
// generators are silent; the Feedback back edge contributes silence.
AudioProgram flatten_audio_program(const Document& doc, uint64_t root_id);

struct MediaInstance {
    uint64_t key = 0;    // hash(path, container id, asset) = GraphNode::key
    uint64_t owner = 0;  // the look or sequence holding the container
    uint64_t layer = 0;  // the layer (looks) or track (sequences) id
    uint64_t asset = 0;
    // The media node's LOCAL CLOCK through the composed affine:
    // source = (root - t_in) * speed + source_in, live on [t_in, t_out).
    // The MEDIA frame behind it is floor(source * rate) + shift.
    double t_in = 0.0;
    double t_out = 0.0;
    double source_in = 0.0;
    double speed = 1.0;
    // Media-hop conform: MEDIA frames per composed clock frame
    // (asset fps / clock fps; 1 for matched rates, stills, unknowns).
    double rate = 1.0;
    // Media-frame-exact in-point: slip + Offset shims, applied AFTER
    // the rate so it never pre-divides through the floor.
    int64_t shift = 0;
    // Composed audio gain: the placement's, scaled by every enclosing
    // track and block, and 0 when anything on the path is muted.
    float gain = 1.0f;
};

// Every media source a picture walk from `root_id` (a look or a sequence)
// reaches, culled by visibility and nesting depth exactly as compile
// culls, in the same emission order. Two blocks of one entity appear
// twice, with different keys. This is the decode pool's walk.
std::vector<MediaInstance> flatten_media_sources(const Document& doc,
                                               uint64_t root_id);

// The LEAF VIEW of the audio program: every media read the program
// reaches, with its composed time map and gain (track * placement *
// everything enclosing; any mute prunes at build). Metering and scope
// analysis follow WHAT SOUNDS through this; the mix plays the program
// itself. A leaf a diamond reaches twice emits once.
std::vector<MediaInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id);

// The audio chain FEEDING a graph node, composed to closed form: walk
// back from `node` (a layer or effect id in `look`) through port-0
// inputs with the same fan-in walk the mix sums, but keeping only the
// bottom-most RESOLVABLE path - analysis is keyed on one media stream
// and beat clocks anchor on media time. Nested look refs resolve
// through their own Output (inner ops first, the leaf's slip carried
// out). asset 0 = nothing resolvable behind the wire (generator,
// unwired, dangling, or a sequence ref - a sequence has no single
// voice). Runtime analysis keys on this.
struct AudioChain {
    uint64_t asset = 0;
    uint32_t slip = 0;
    // Summed Offset-node shift along the chain (audio-targeting nodes
    // sitting directly on their source, every nesting hop): the media
    // frame behind local L is floor(L * rate) + slip + offset.
    int64_t offset = 0;
    // Media-hop conform ratio of the chain's asset (media frames per
    // clock frame; 1 for matched rates, stills, unknowns).
    double rate = 1.0;
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

// The media node's local-clock position at a root frame - the affine
// alone, no rate, no shift.
inline double media_source_frame(const MediaInstance& c, double root_frame) {
    return (root_frame - c.t_in) * c.speed + c.source_in;
}

// The MEDIA frame this instance shows at a root frame: the clock
// position conformed by rate, floored, then the media-frame-exact
// shift - so slip lands on exact media frames at any rate. Callers
// clamp into the asset - a window may outlive its media.
inline double media_asset_frame(const MediaInstance& c, double root_frame) {
    return std::floor(media_source_frame(c, root_frame) * c.rate) +
           static_cast<double>(c.shift);
}

}  // namespace looks::doc
