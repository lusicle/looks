#include "doc/instances.h"

#include <algorithm>

#include "util/hash.h"

namespace looks::doc {

namespace {

// A pathological file (deep nesting times wide layer counts) must
// terminate: the depth guard alone allows kMaxLayers^kMaxLookDepth.
constexpr size_t kMaxFlattened = 1024;

// One instance being descended. `local = a * root + b` is the composed
// affine clock; [r0, r1) is the root span the instance is live on.
struct Cursor {
    uint64_t entity = 0;   // a look or a sequence
    uint64_t path = 0;
    int depth = 0;
    double a = 1.0, b = 0.0;
    double r0 = 0.0, r1 = kUnbounded;
    float gain = 1.0f;
};

// Clamps the cursor's root window to a child live on LOCAL [lo, hi)
// (hi = kUnbounded for none), and composes the child clock through a
// placement-shaped step (speed, source_in). Returns false when the
// window closes. Both bounds are whole frames, so the continuous test
// agrees with the compiler's floored one exactly.
bool child_window(const Cursor& cur, double lo, double hi, double speed,
                  double source_in, Cursor* child) {
    double r0 = cur.r0, r1 = cur.r1;
    if (cur.a > 0.0) {
        r0 = std::max(r0, (lo - cur.b) / cur.a);
        if (hi < kUnbounded) r1 = std::min(r1, (hi - cur.b) / cur.a);
    } else if (cur.b < lo || cur.b >= hi) {
        return false;   // frozen chain, parked outside the window
    }
    if (r1 <= r0) return false;
    child->depth = cur.depth + 1;
    child->a = cur.a * speed;
    child->b = (cur.b - lo) * speed + source_in;
    child->r0 = r0;
    child->r1 = r1;
    return true;
}

void walk(const Document& doc, const Cursor& cur, bool audio,
          std::vector<ClipInstance>& out);

// A look's sources run in LOCKSTEP: identity clock, windowed only by
// what the source can play. Clips add their slip; nested entities pass
// time straight through, cut by an explicit duration when one is set.
void walk_look(const Document& doc, const Look& look, const Cursor& cur,
               bool audio, std::vector<ClipInstance>& out) {
    for (const Layer& layer : look.layers) {
        if (!audio && !layer.visible) continue;
        if (out.size() >= kMaxFlattened) return;
        if (layer_is_clip(layer)) {
            if (!layer.asset) continue;
            const Asset* a = doc.find_asset(layer.asset);
            const uint32_t frames = a ? a->frame_count : 0;
            const double hi =
                frames > layer.slip
                    ? static_cast<double>(frames - layer.slip)
                    : (frames ? 0.0 : kUnbounded);
            Cursor leaf;
            if (!child_window(cur, 0.0, hi, 1.0,
                              static_cast<double>(layer.slip), &leaf))
                continue;
            ClipInstance c;
            // Container AND asset fold into the key, matching the
            // compiler's Source stamp.
            c.key = hash_combine(hash_combine(cur.path, layer.id),
                                 layer.asset);
            c.owner = look.id;
            c.layer = layer.id;
            c.asset = layer.asset;
            c.speed = leaf.a;
            c.t_in = leaf.r0;
            c.t_out = leaf.r1;
            c.source_in = leaf.r0 * leaf.a + leaf.b;
            c.gain = cur.gain;
            out.push_back(c);
            continue;
        }
        if (!layer_is_nested(layer) || !layer.target) continue;
        if (cur.depth + 1 >= kMaxLookDepth) continue;
        // An explicit duration cuts the nested entity; a derived one
        // equals its content bounds, so only the explicit case clamps.
        double hi = kUnbounded;
        if (const Look* t = doc.find_look(layer.target)) {
            if (t->duration) hi = static_cast<double>(t->duration);
        } else if (const Sequence* t = doc.find_sequence(layer.target)) {
            if (t->duration) hi = static_cast<double>(t->duration);
        } else {
            continue;   // dangling ref: dormant
        }
        Cursor child;
        if (!child_window(cur, 0.0, hi, 1.0, 0.0, &child)) continue;
        child.entity = layer.target;
        child.path = hash_combine(hash_combine(cur.path, layer.id),
                                  layer.target);
        child.gain = cur.gain;
        walk(doc, child, audio, out);
    }
}

// A sequence's placements are the affine hops. The picture walk reads
// the video lanes; the audio walk reads the audio tracks - video lanes
// are silent, sound rides audio placements only.
void walk_sequence(const Document& doc, const Sequence& seq,
                   const Cursor& cur, bool audio,
                   std::vector<ClipInstance>& out) {
    auto descend = [&](const Placement& p, uint64_t container,
                       float base_gain) {
        if (out.size() >= kMaxFlattened) return;
        if (!p.target) return;
        if (cur.depth + 1 >= kMaxLookDepth) return;
        if (!doc.find_look(p.target) && !doc.find_sequence(p.target))
            return;   // dangling target: dormant
        const double speed =
            p.speed > 0.0f ? static_cast<double>(p.speed) : 0.0;
        const uint32_t len = source_length(doc, p);
        const uint32_t end = placement_end(p, len);
        const double lo = static_cast<double>(p.t_in);
        const double hi = end ? static_cast<double>(end) : kUnbounded;
        Cursor child;
        if (!child_window(cur, lo, hi, speed,
                          static_cast<double>(p.source_in), &child))
            return;
        child.entity = p.target;
        child.path = hash_combine(hash_combine(cur.path, container),
                                  p.target);
        child.gain = p.audio_mute
            ? 0.0f
            : base_gain * std::max(p.audio_gain, 0.0f);
        if (audio && child.gain <= 0.0f) return;
        walk(doc, child, audio, out);
    };

    if (!audio) {
        for (const SeqTrack& t : seq.tracks)
            for (const Placement& p : t.placements)
                descend(p, t.id, cur.gain);
        return;
    }
    for (const AudioTrack& t : seq.audio) {
        const float track_gain =
            t.mute ? 0.0f : cur.gain * std::max(t.gain, 0.0f);
        if (track_gain <= 0.0f) continue;
        for (const Placement& p : t.placements)
            descend(p, t.id, track_gain);
    }
}

void walk(const Document& doc, const Cursor& cur, bool audio,
          std::vector<ClipInstance>& out) {
    if (const Look* look = doc.find_look(cur.entity)) {
        walk_look(doc, *look, cur, audio, out);
        return;
    }
    if (const Sequence* seq = doc.find_sequence(cur.entity))
        walk_sequence(doc, *seq, cur, audio, out);
}

std::vector<ClipInstance> flatten(const Document& doc, uint64_t root_id,
                                  bool audio) {
    std::vector<ClipInstance> out;
    Cursor root;
    root.entity = root_id;
    root.path = root_id;   // the root instance's path is its own id
    walk(doc, root, audio, out);
    return out;
}

}  // namespace

std::vector<ClipInstance> flatten_clip_sources(const Document& doc,
                                               uint64_t root_id) {
    return flatten(doc, root_id, /*audio=*/false);
}

std::vector<ClipInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id) {
    return flatten(doc, root_id, /*audio=*/true);
}

}  // namespace looks::doc
