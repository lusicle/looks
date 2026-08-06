// Instance flatten: the nesting tree composed down to its LEAF CLIP
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

namespace looks::doc {

// A window whose end is the parent's end rather than its own.
inline constexpr double kUnbounded = 1e18;

struct ClipInstance {
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
};

// Every clip a picture walk from `root_id` (a look or a sequence)
// reaches, culled by visibility and nesting depth exactly as compile
// culls, in the same emission order. Two blocks of one entity appear
// twice, with different keys. This is the decode pool's walk.
std::vector<ClipInstance> flatten_clip_sources(const Document& doc,
                                               uint64_t root_id);

// The AUDIO walk: sequences contribute their AUDIO TRACKS' placements
// (video lanes are silent - sound rides audio placements only), and a
// look contributes its clip nodes' PCM in lockstep. Nested entities
// recurse either way. Gain composes as track gain * placement gain *
// everything enclosing; any mute on the path is 0.
std::vector<ClipInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id);

inline bool clip_active(const ClipInstance& c, double root_frame) {
    return root_frame >= c.t_in && root_frame < c.t_out;
}

// The asset frame this clip shows at a root frame. Callers clamp into
// the asset - a window may outlive its media.
inline double clip_source_frame(const ClipInstance& c, double root_frame) {
    return (root_frame - c.t_in) * c.speed + c.source_in;
}

}  // namespace looks::doc
