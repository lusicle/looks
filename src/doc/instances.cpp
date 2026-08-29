#include "doc/instances.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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
    // local = a * root + b maps ROOT frames onto THIS entity's OWN
    // clock: every nesting hop folds its fps ratio into the affine, so
    // pinned entities tick their own rate and the flatten stays one
    // closed form.
    double a = 1.0, b = 0.0;
    double r0 = 0.0, r1 = kUnbounded;
    // The ROOT entity's effective rate: TIMELINE-LOCKED media reads the
    // root clock, so its conform ratio is against this.
    double root_fps = 30.0;
};

// Clamps the cursor's root window to a child live on LOCAL [lo, hi)
// (hi = kUnbounded for none), and composes the child clock through a
// placement-shaped step (speed, source_in). Returns false when the
// window closes. The flatten and the compiler test the SAME real
// bounds (conformed media windows may be fractional), so the
// continuous test and the point test agree exactly.
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

void walk(const Document& doc, const Cursor& cur,
          std::vector<MediaInstance>& out);

// The bottom LIVE feed into (to, port): the first link whose producer
// still exists - link-vector order IS stacking order. Single-answer
// questions (Offset adjacency, analysis chains) resolve through this;
// the full fan-in walks every live feed.
uint64_t link_into(const Look& look, const std::vector<NodeLink>& links,
                   uint64_t to, uint32_t port) {
    for (const NodeLink& l : links)
        if (l.to == to && l.to_port == port &&
            wire_producer_live(look, l.from))
            return l.from;
    return 0;
}

// One resolved chain of a look's audio: a root layer plus the DSP hops
// between it and the walk's start, in play order (source first). The
// ANALYSIS view - the mix plays the program, analysis listens to one
// stream.
struct VoicePath {
    const Layer* root = nullptr;
    std::vector<AudioOp> ops;
    int64_t audio_off = 0;   // Offset shims sitting on the path's source
};

// Fan-in paths cap deterministically, and the visit budget bounds the
// walk through the one legal cycle (Feedback passes audio through).
constexpr size_t kMaxVoicePaths = 64;
constexpr int kVoiceVisitBudget = 4096;
constexpr int kMaxVoiceDepth = 256;

// Walks back from `cur` (inclusive) through port-0 fan-ins, following
// EVERY live feed - the same enumeration the compiler merges and the
// audio program sums; this path view exists for single-stream
// consumers. Audio-modifier hops (not bypassed, group live) ride each
// path; every other node passes audio through; group input slots
// splice to their exterior fan-in. A path ends at a layer; a dangling
// or unfed hop is a silent dead end. Paths emit bottom-first, so
// paths.front() is the bottom-most resolvable chain. `rev` holds the
// hops output-first while recursing; each emit copies the first
// kMaxVoiceOps (the hops nearest the output win).
void walk_paths(const Look& look, const std::vector<NodeLink>& links,
                uint64_t cur, std::vector<AudioOp>& rev, int64_t off,
                int depth, int& budget, std::vector<VoicePath>& out) {
    if (!cur || out.size() >= kMaxVoicePaths || depth > kMaxVoiceDepth ||
        budget-- <= 0)
        return;
    if (const Layer* layer = find_layer(look, cur)) {
        VoicePath p;
        p.root = layer;
        p.audio_off = off;
        const size_t n = std::min(rev.size(), kMaxVoiceOps);
        p.ops.reserve(n);
        for (size_t i = 0; i < n; ++i) p.ops.push_back(rev[n - 1 - i]);
        out.push_back(std::move(p));
        return;
    }
    const Layer* owner = nullptr;
    const EffectInstance* fx = find_effect(look, cur, &owner);
    if (!fx) {
        if (!group_of_input(look, cur)) return;   // dangling id: silent
        for (const NodeLink& l : links)
            if (l.to == cur && l.to_port == 0 &&
                wire_producer_live(look, l.from))
                walk_paths(look, links, l.from, rev, off, depth + 1,
                           budget, out);
        return;
    }
    const bool live =
        !fx->bypass && !group_bypassed(*owner, fx->group_id);
    bool pushed = false;
    if (is_audio_effect(fx->type) && live) {
        AudioOp op;
        op.type = fx->type;
        const size_t n = std::min<size_t>(fx->params.size(), 4);
        for (size_t i = 0; i < n; ++i) op.params[i] = fx->params[i];
        op.wet = fx->wet;
        rev.push_back(op);
        pushed = true;
    }
    for (const NodeLink& l : links) {
        if (l.to != fx->id || l.to_port != 0 ||
            !wire_producer_live(look, l.from))
            continue;
        // Source-adjacency looks THROUGH slots: an Offset just inside
        // a group boundary still sits directly on the wired source.
        // The shim rides only the branches whose feed IS a source.
        int64_t branch_off = off;
        if (fx->type == EffectType::Offset && live &&
            offset_targets_audio(*fx) &&
            find_layer(look, hop_group_inputs(look, links, l.from)))
            branch_off += offset_frames(*fx);
        walk_paths(look, links, l.from, rev, branch_off, depth + 1,
                   budget, out);
    }
    if (pushed) rev.pop_back();
}

// Every chain wired into the look's Output, bottom-first: combined =
// the In fan-in on port 0, split = the dedicated audio-in on port 1
// (unwired = silent, never a fallback).
std::vector<VoicePath> resolve_voices(const Look& look) {
    std::vector<NodeLink> synth;
    const std::vector<NodeLink>& links = effective_links(look, synth);
    std::vector<VoicePath> out;
    std::vector<AudioOp> rev;
    int budget = kVoiceVisitBudget;
    const uint32_t port = look.audio_split ? 1u : 0u;
    for (const NodeLink& l : links)
        if (l.to == 0 && l.to_port == port &&
            wire_producer_live(look, l.from))
            walk_paths(look, links, l.from, rev, 0, 0, budget, out);
    return out;
}

// ------------------------------------------------- audio program build
//
// The audio walk builds the wired graph itself, flattened: nodes sum
// their fan-in, ops process the sum on their look's clock, hops window
// and gain their subtree. The same cursor composition the picture walk
// uses folds every nesting hop to closed form.

struct ProgBuild {
    const Document& doc;
    AudioProgram& prog;
};

int build_entity_audio(ProgBuild& pb, uint64_t entity, const Cursor& cur);

int alloc_audio_node(ProgBuild& pb) {
    if (pb.prog.nodes.size() >= kMaxAudioNodes) return -1;
    pb.prog.nodes.emplace_back();
    return static_cast<int>(pb.prog.nodes.size()) - 1;
}

// Per-look-instance build state: one memo per instance (every nesting
// hop composes its own clock), and a build stack that turns the one
// legal cycle (Feedback) into a silent back edge.
struct LookBuild {
    ProgBuild& pb;
    const Look& look;
    const std::vector<NodeLink>& links;
    const Cursor& cur;
    double eff;
    std::unordered_map<uint64_t, int> memo;
    std::unordered_set<uint64_t> building;
};

int build_doc_node(LookBuild& lb, uint64_t id, int64_t src_shift);

// A layer as a program node: media = a leaf read, nested = a windowed
// hop over the child entity's program, generator = silence. src_shift
// is an audio-targeting Offset sitting directly on this source.
int build_layer_audio(LookBuild& lb, const Layer& layer,
                      int64_t src_shift) {
    const Document& doc = lb.pb.doc;
    const Cursor& cur = lb.cur;
    if (layer_is_media(layer)) {
        if (!layer.asset) return -1;
        const Asset* a = doc.find_asset(layer.asset);
        if (!a) return -1;   // dangling id: dormant, like the picture
        const int64_t shift =
            static_cast<int64_t>(layer.slip) + src_shift;
        const double rate = media_conform_rate(
            doc, *a, layer.timeline_lock ? cur.root_fps : lb.eff);
        double lo = 0.0, hi = 0.0;
        shifted_window(static_cast<double>(a->frame_count), shift, rate,
                       &lo, &hi);
        Cursor leaf;
        if (layer.timeline_lock) {
            // The node reads the asset at the ROOT clock: identity
            // map, ignoring every composed placement hop.
            leaf.depth = cur.depth + 1;
            leaf.a = 1.0;
            leaf.b = 0.0;
            leaf.r0 = std::max(cur.r0, lo);
            leaf.r1 = std::min(cur.r1, hi);
            if (leaf.r1 <= leaf.r0) return -1;
        } else if (!child_window(cur, lo, hi, 1.0, lo, &leaf)) {
            return -1;
        }
        const int idx = alloc_audio_node(lb.pb);
        if (idx < 0) return -1;
        AudioNode& n = lb.pb.prog.nodes[static_cast<size_t>(idx)];
        n.asset = layer.asset;
        // Matching the compiler's Source stamp (media_stream_key - one
        // formula); only the Offset shim folds in, never the slip.
        n.key = media_stream_key(cur.path, layer.id, layer.asset,
                                 layer.timeline_lock, src_shift);
        n.owner = lb.look.id;
        n.layer = layer.id;
        n.doc_id = layer.id;
        n.a = leaf.a;
        n.b = leaf.b;
        n.local_fps = layer.timeline_lock ? cur.root_fps : lb.eff;
        n.rate = rate;
        n.shift = shift;
        // Leaf spans feed the LEAF VIEW; the mix tapers on PCM bounds.
        n.w0 = leaf.r0;
        n.w1 = leaf.r1;
        return idx;
    }
    if (layer_is_nested(layer)) {
        if (!layer.target) return -1;
        if (cur.depth + 1 >= kMaxLookDepth) return -1;
        // An explicit duration cuts the nested entity; a derived one
        // equals its content bounds, so only the explicit case clamps.
        double dur = 0.0;
        if (const Look* t = doc.find_look(layer.target)) {
            dur = static_cast<double>(t->duration);
        } else if (const Sequence* t = doc.find_sequence(layer.target)) {
            dur = static_cast<double>(t->duration);
        } else {
            return -1;   // dangling ref: dormant
        }
        const double ratio = entity_fps(doc, layer.target) / lb.eff;
        double lo = 0.0, hi = 0.0;
        shifted_window(dur, src_shift, ratio, &lo, &hi);
        Cursor child;
        if (!child_window(cur, lo, hi, ratio,
                          lo * ratio + static_cast<double>(src_shift),
                          &child))
            return -1;
        child.entity = layer.target;
        child.path = nested_child_path(cur.path, layer.id, layer.target,
                                       src_shift);
        child.root_fps = cur.root_fps;
        const int inner = build_entity_audio(lb.pb, layer.target, child);
        if (inner < 0) return -1;
        const int idx = alloc_audio_node(lb.pb);
        if (idx < 0) return -1;
        AudioNode& n = lb.pb.prog.nodes[static_cast<size_t>(idx)];
        n.windowed = true;
        n.w0 = child.r0;
        n.w1 = child.r1;
        n.inputs.push_back(inner);
        return idx;
    }
    return -1;   // generator: silence
}

// A doc node's contribution: layers end the wire, audio effects wrap
// their fan-in sum in an op, everything else passes the sum through.
// -1 = silence (dangling, unfed, generator, or the Feedback back edge).
int build_doc_node(LookBuild& lb, uint64_t id, int64_t src_shift) {
    if (!src_shift) {
        const auto it = lb.memo.find(id);
        if (it != lb.memo.end()) return it->second;
    }
    if (lb.building.count(id)) return -1;   // Feedback edge: silence
    lb.building.insert(id);
    int result = -1;
    if (const Layer* layer = find_layer(lb.look, id)) {
        result = build_layer_audio(lb, *layer, src_shift);
    } else {
        const Layer* owner = nullptr;
        const EffectInstance* fx = find_effect(lb.look, id, &owner);
        if (fx) {
            const bool live =
                !fx->bypass && !group_bypassed(*owner, fx->group_id);
            // An audio-targeting Offset shifts the sources it sits
            // DIRECTLY on (slots forward the shim; any other hop
            // drops it - adjacency is per branch).
            int64_t feed_shift = 0;
            if (fx->type == EffectType::Offset && live &&
                offset_targets_audio(*fx))
                feed_shift = offset_frames(*fx);
            std::vector<int> children;
            for (const NodeLink& l : lb.links) {
                if (l.to != id || l.to_port != 0 ||
                    !wire_producer_live(lb.look, l.from))
                    continue;
                const int c = build_doc_node(lb, l.from, feed_shift);
                if (c >= 0) children.push_back(c);
            }
            if (children.empty()) {
                result = -1;
            } else if (is_audio_effect(fx->type) && live) {
                const int idx = alloc_audio_node(lb.pb);
                if (idx >= 0) {
                    AudioNode& n =
                        lb.pb.prog.nodes[static_cast<size_t>(idx)];
                    n.doc_id = fx->id;
                    n.a = lb.cur.a;
                    n.b = lb.cur.b;
                    n.local_fps = lb.eff;
                    n.has_op = true;
                    n.op.type = fx->type;
                    const size_t np =
                        std::min<size_t>(fx->params.size(), 4);
                    for (size_t i = 0; i < np; ++i)
                        n.op.params[i] = fx->params[i];
                    n.op.wet = fx->wet;
                    n.inputs = std::move(children);
                }
                result = idx;
            } else if (children.size() == 1) {
                result = children[0];
            } else {
                const int idx = alloc_audio_node(lb.pb);
                if (idx >= 0)
                    lb.pb.prog.nodes[static_cast<size_t>(idx)].inputs =
                        std::move(children);
                result = idx;
            }
        } else if (group_of_input(lb.look, id)) {
            std::vector<int> children;
            for (const NodeLink& l : lb.links) {
                if (l.to != id || l.to_port != 0 ||
                    !wire_producer_live(lb.look, l.from))
                    continue;
                const int c = build_doc_node(lb, l.from, src_shift);
                if (c >= 0) children.push_back(c);
            }
            if (children.size() == 1) {
                result = children[0];
            } else if (!children.empty()) {
                const int idx = alloc_audio_node(lb.pb);
                if (idx >= 0)
                    lb.pb.prog.nodes[static_cast<size_t>(idx)].inputs =
                        std::move(children);
                result = idx;
            }
        }
    }
    lb.building.erase(id);
    if (!src_shift) lb.memo[id] = result;
    return result;
}

int build_look_audio(ProgBuild& pb, const Look& look, const Cursor& cur) {
    std::vector<NodeLink> synth;
    const std::vector<NodeLink>& links = effective_links(look, synth);
    LookBuild lb{pb,  look, links, cur,
                 effective_fps(pb.doc, look), {}, {}};
    const uint32_t port = look.audio_split ? 1u : 0u;
    std::vector<int> children;
    for (const NodeLink& l : links) {
        if (l.to != 0 || l.to_port != port ||
            !wire_producer_live(look, l.from))
            continue;
        const int c = build_doc_node(lb, l.from, 0);
        if (c >= 0) children.push_back(c);
    }
    if (children.empty()) return -1;
    if (children.size() == 1) return children[0];
    const int idx = alloc_audio_node(pb);
    if (idx < 0) return -1;
    pb.prog.nodes[static_cast<size_t>(idx)].inputs = std::move(children);
    return idx;
}

// A sequence's audio is its AUDIO TRACKS' placements - each a windowed,
// gained hop over its target's program (video lanes are silent).
int build_seq_audio(ProgBuild& pb, const Sequence& seq,
                    const Cursor& cur) {
    const double eff = effective_fps(pb.doc, seq);
    std::vector<int> children;
    for (const AudioTrack& t : seq.audio) {
        const float track_gain = t.mute ? 0.0f : std::max(t.gain, 0.0f);
        if (track_gain <= 0.0f) continue;
        for (const Placement& p : t.placements) {
            if (!p.target) continue;
            if (cur.depth + 1 >= kMaxLookDepth) continue;
            if (!pb.doc.find_look(p.target) &&
                !pb.doc.find_sequence(p.target))
                continue;   // dangling block: dormant
            const float pgain =
                p.audio_mute
                    ? 0.0f
                    : track_gain * std::max(p.audio_gain, 0.0f);
            if (pgain <= 0.0f) continue;
            const double ratio = placement_ratio(pb.doc, p, eff);
            const double speed =
                p.speed > 0.0f ? static_cast<double>(p.speed) * ratio
                               : 0.0;
            const uint32_t len = source_length(pb.doc, p);
            const uint32_t end = placement_end(p, len, ratio);
            const double lo = static_cast<double>(p.t_in);
            const double hi =
                end ? static_cast<double>(end) : kUnbounded;
            Cursor child;
            if (!child_window(cur, lo, hi, speed,
                              static_cast<double>(p.source_in), &child))
                continue;
            child.entity = p.target;
            child.path = seq_child_path(cur.path, t.id, p.target);
            child.root_fps = cur.root_fps;
            const int inner = build_entity_audio(pb, p.target, child);
            if (inner < 0) continue;
            const int idx = alloc_audio_node(pb);
            if (idx < 0) break;
            AudioNode& n = pb.prog.nodes[static_cast<size_t>(idx)];
            n.windowed = true;
            n.w0 = child.r0;
            n.w1 = child.r1;
            n.gain = pgain;
            n.inputs.push_back(inner);
            children.push_back(idx);
        }
    }
    if (children.empty()) return -1;
    if (children.size() == 1) return children[0];
    const int idx = alloc_audio_node(pb);
    if (idx < 0) return -1;
    pb.prog.nodes[static_cast<size_t>(idx)].inputs = std::move(children);
    return idx;
}

int build_entity_audio(ProgBuild& pb, uint64_t entity, const Cursor& cur) {
    if (const Look* look = pb.doc.find_look(entity))
        return build_look_audio(pb, *look, cur);
    if (const Sequence* seq = pb.doc.find_sequence(entity))
        return build_seq_audio(pb, *seq, cur);
    return -1;
}

// ---------------------------------------------------------- picture walk

// A look's sources run in LOCKSTEP: identity clock, windowed only by
// what the source can play. Media layers add their slip; nested
// entities pass time straight through, cut by an explicit duration when
// one is set. The picture walk emits every visible wired-or-not layer
// (compile culls by wiring itself).
void walk_look(const Document& doc, const Look& look, const Cursor& cur,
               std::vector<MediaInstance>& out) {
    const double eff = effective_fps(doc, look);
    auto emit_media = [&](const Layer& layer, int64_t off) {
        if (!layer.asset) return;
        const Asset* a = doc.find_asset(layer.asset);
        // A DANGLING id (asset removed) is dormant exactly like an
        // unbound node. An asset with no picture (audio import without
        // cover art: no frames, no dimensions) has no image side.
        if (!a) return;
        if (!a->frame_count && !a->width && !a->height) return;
        const uint32_t frames = a->frame_count;
        const int64_t shift = static_cast<int64_t>(layer.slip) + off;
        // The cursor affine already lands in this LOOK's own clock, so
        // the media ratio is against the look's effective rate; a
        // LOCKED node reads the ROOT clock instead.
        const double rate = media_conform_rate(
            doc, *a, layer.timeline_lock ? cur.root_fps : eff);
        double lo = 0.0, hi = 0.0;
        shifted_window(static_cast<double>(frames), shift, rate, &lo, &hi);
        Cursor leaf;
        if (layer.timeline_lock) {
            // The node reads the asset at the ROOT clock: identity map,
            // ignoring every composed placement hop. The window bounds
            // are the same formulas read in root frames.
            leaf.depth = cur.depth + 1;
            leaf.a = 1.0;
            leaf.b = 0.0;
            leaf.r0 = std::max(cur.r0, lo);
            leaf.r1 = std::min(cur.r1, hi);
            if (leaf.r1 <= leaf.r0) return;
        } else if (!child_window(cur, lo, hi, 1.0, lo, &leaf)) {
            return;
        }
        MediaInstance c;
        // Container, asset, lock and shift fold into the key, matching
        // the compiler's Source stamp (media_stream_key - one formula).
        c.key = media_stream_key(cur.path, layer.id, layer.asset,
                                 layer.timeline_lock, off);
        c.owner = look.id;
        c.layer = layer.id;
        c.asset = layer.asset;
        c.speed = leaf.a;
        c.t_in = leaf.r0;
        c.t_out = leaf.r1;
        c.source_in = leaf.r0 * leaf.a + leaf.b;
        c.rate = rate;
        c.shift = shift;
        out.push_back(c);
    };
    auto descend_nested = [&](const Layer& layer, int64_t off) {
        if (!layer.target) return;
        if (cur.depth + 1 >= kMaxLookDepth) return;
        // An explicit duration cuts the nested entity; a derived one
        // equals its content bounds, so only the explicit case clamps.
        // LOCKSTEP means 1:1 IN TIME: the hop's fps ratio scales the
        // child clock; the shift (child frames) moves it.
        double dur = 0.0;
        if (const Look* t = doc.find_look(layer.target)) {
            dur = static_cast<double>(t->duration);
        } else if (const Sequence* t = doc.find_sequence(layer.target)) {
            dur = static_cast<double>(t->duration);
        } else {
            return;   // dangling ref: dormant
        }
        const double ratio = entity_fps(doc, layer.target) / eff;
        double lo = 0.0, hi = 0.0;
        shifted_window(dur, off, ratio, &lo, &hi);
        Cursor child;
        if (!child_window(cur, lo, hi, ratio,
                          lo * ratio + static_cast<double>(off), &child))
            return;
        child.entity = layer.target;
        child.path = nested_child_path(cur.path, layer.id, layer.target,
                                       off);
        child.root_fps = cur.root_fps;
        walk(doc, child, out);
    };

    // Live video-targeting Offset shims sitting DIRECTLY on a source
    // (port-0 input is the layer): each adds a shifted read of that
    // source next to the base one. Liveness mirrors compile's
    // effect_active exactly - owner layer visible, not bypassed, not
    // muted by a solo elsewhere in its layer, group live.
    std::vector<std::pair<uint64_t, int64_t>> vshifts;
    {
        std::vector<NodeLink> synth;
        const std::vector<NodeLink>& links = effective_links(look, synth);
        for (const Layer& holder : look.layers) {
            if (!holder.visible) continue;
            bool any_solo = false;
            for (const EffectInstance& fx : holder.stack)
                if (fx.solo && !fx.bypass) any_solo = true;
            for (const EffectInstance& fx : holder.stack) {
                if (fx.type != EffectType::Offset || fx.bypass) continue;
                if (any_solo && !fx.solo) continue;
                if (group_bypassed(holder, fx.group_id)) continue;
                if (!offset_targets_video(fx)) continue;
                const int64_t off = offset_frames(fx);
                if (!off) continue;
                // Adjacency looks THROUGH group input slots, matching
                // the compiler's shim pass.
                const uint64_t src = hop_group_inputs(
                    look, links, link_into(look, links, fx.id, 0));
                for (const Layer& l : look.layers)
                    if (l.id == src) vshifts.emplace_back(src, off);
            }
        }
    }
    for (const Layer& layer : look.layers) {
        if (!layer.visible) continue;
        if (out.size() >= kMaxFlattened) return;
        if (layer_is_media(layer)) {
            emit_media(layer, 0);
        } else if (layer_is_nested(layer)) {
            descend_nested(layer, 0);
        } else {
            continue;
        }
        for (const auto& [src, off] : vshifts) {
            if (src != layer.id) continue;
            if (out.size() >= kMaxFlattened) return;
            if (layer_is_media(layer))
                emit_media(layer, off);
            else
                descend_nested(layer, off);
        }
    }
}

// A sequence's placements are the affine hops. The picture walk reads
// the video lanes only - sound is the audio program's business.
void walk_sequence(const Document& doc, const Sequence& seq,
                   const Cursor& cur, std::vector<MediaInstance>& out) {
    const double eff = effective_fps(doc, seq);
    // Hidden lanes leave the composite entirely - the flatten and
    // the compiler must agree or the pool prewarms ghosts.
    for (const SeqTrack& t : seq.tracks) {
        if (t.hidden) continue;
        for (const Placement& p : t.placements) {
            if (out.size() >= kMaxFlattened) return;
            if (!p.target) continue;
            if (cur.depth + 1 >= kMaxLookDepth) continue;
            if (!doc.find_look(p.target) && !doc.find_sequence(p.target))
                continue;   // dangling target: dormant
            const double ratio = placement_ratio(doc, p, eff);
            const double speed =
                p.speed > 0.0f ? static_cast<double>(p.speed) * ratio
                               : 0.0;
            const uint32_t len = source_length(doc, p);
            const uint32_t end = placement_end(p, len, ratio);
            const double lo = static_cast<double>(p.t_in);
            const double hi =
                end ? static_cast<double>(end) : kUnbounded;
            Cursor child;
            if (!child_window(cur, lo, hi, speed,
                              static_cast<double>(p.source_in), &child))
                continue;
            child.entity = p.target;
            child.path = seq_child_path(cur.path, t.id, p.target);
            child.root_fps = cur.root_fps;
            walk(doc, child, out);
        }
    }
}

void walk(const Document& doc, const Cursor& cur,
          std::vector<MediaInstance>& out) {
    if (const Look* look = doc.find_look(cur.entity)) {
        walk_look(doc, *look, cur, out);
        return;
    }
    if (const Sequence* seq = doc.find_sequence(cur.entity))
        walk_sequence(doc, *seq, cur, out);
}

Cursor root_cursor(const Document& doc, uint64_t root_id) {
    Cursor root;
    root.entity = root_id;
    root.path = root_id;   // the root instance's path is its own id
    root.root_fps = entity_fps(doc, root_id);
    return root;
}

}  // namespace

std::vector<MediaInstance> flatten_media_sources(const Document& doc,
                                               uint64_t root_id) {
    std::vector<MediaInstance> out;
    walk(doc, root_cursor(doc, root_id), out);
    return out;
}

AudioProgram flatten_audio_program(const Document& doc, uint64_t root_id) {
    AudioProgram prog;
    ProgBuild pb{doc, prog};
    prog.root = build_entity_audio(pb, root_id, root_cursor(doc, root_id));
    return prog;
}

std::vector<MediaInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id) {
    const AudioProgram prog = flatten_audio_program(doc, root_id);
    std::vector<MediaInstance> out;
    if (prog.root < 0) return out;
    // Depth-first, inputs in stored order (bottom chain first),
    // composing hop gains down; a node a diamond reaches twice emits
    // once, with its first path's gain.
    std::vector<char> seen(prog.nodes.size(), 0);
    std::vector<std::pair<int, float>> stack;
    stack.emplace_back(prog.root, 1.0f);
    while (!stack.empty()) {
        const auto [idx, gain] = stack.back();
        stack.pop_back();
        if (seen[static_cast<size_t>(idx)]) continue;
        seen[static_cast<size_t>(idx)] = 1;
        const AudioNode& n = prog.nodes[static_cast<size_t>(idx)];
        const float g = gain * n.gain;
        if (n.asset) {
            if (out.size() >= kMaxFlattened) break;
            MediaInstance c;
            c.key = n.key;
            c.owner = n.owner;
            c.layer = n.layer;
            c.asset = n.asset;
            c.t_in = n.w0;
            c.t_out = n.w1;
            c.source_in = n.a * n.w0 + n.b;
            c.speed = n.a;
            c.rate = n.rate;
            c.shift = n.shift;
            c.gain = g;
            out.push_back(c);
            continue;
        }
        for (auto it = n.inputs.rbegin(); it != n.inputs.rend(); ++it)
            stack.emplace_back(*it, g);
    }
    return out;
}

AudioChain resolve_audio_chain(const Document& doc, const Look& look,
                               uint64_t node) {
    AudioChain out;
    std::vector<AudioOp> chain;   // play order, deepest hops first
    int64_t off = 0;              // Offset shims sum across the hops
    // Analysis is keyed on ONE media stream (beat clocks anchor on
    // media time), so a fan-in resolves to its bottom-most RESOLVABLE
    // path - paths.front() of the same walk the program sums whole.
    std::vector<VoicePath> paths;
    {
        std::vector<NodeLink> synth;
        const std::vector<NodeLink>& links = effective_links(look, synth);
        std::vector<AudioOp> rev;
        int budget = kVoiceVisitBudget;
        walk_paths(look, links, node, rev, 0, 0, budget, paths);
    }
    for (int depth = 0; depth < kMaxLookDepth; ++depth) {
        if (paths.empty()) return {};
        const VoicePath& p = paths.front();
        chain.insert(chain.begin(), p.ops.begin(), p.ops.end());
        off += p.audio_off;
        const Layer& root = *p.root;
        if (layer_is_media(root)) {
            // Unbound or dangling (asset removed): no voice, like the
            // media walks.
            if (!root.asset || !doc.find_asset(root.asset)) return {};
            out.asset = root.asset;
            out.slip = root.slip;
            out.offset = off;
            // Lockstep hops are 1:1 in TIME, so the composed ratio
            // telescopes to asset rate over the STARTING look's clock.
            out.rate = media_conform_rate(doc, *doc.find_asset(root.asset),
                                          effective_fps(doc, look));
            out.locked = root.timeline_lock;
            out.op_count = static_cast<uint32_t>(
                std::min(chain.size(), kMaxVoiceOps));
            // Prepends put play order in place; on overflow the hops
            // nearest the walk's start win (the tail).
            const size_t base = chain.size() - out.op_count;
            for (uint32_t i = 0; i < out.op_count; ++i)
                out.ops[i] = chain[base + i];
            return out;
        }
        if (!layer_is_nested(root) || !root.target) return {};
        const Look* t = doc.find_look(root.target);
        if (!t) return {};   // sequence ref: no single voice
        paths = resolve_voices(*t);
    }
    return {};
}

}  // namespace looks::doc
