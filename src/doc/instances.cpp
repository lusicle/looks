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
    // Audio walk: DSP hops accumulated OUTSIDE this instance, appended
    // after each emitted voice's own chain (inner ops run first). The
    // picture walk leaves it empty.
    std::vector<AudioOp> ops;
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
          std::vector<MediaInstance>& out);

// A look's VOICE: walk back from the Output's audio feed (combined =
// the In wire, where the first port-0 link is the bottom chain and wins
// the fan-in; split = the dedicated audio-in on port 1, silent when
// unwired). Audio-modifier hops (not bypassed, group live) collect in
// source-first order; every other node passes audio through on its
// port-0 input. The chain ends at a layer: media is the voice, a nested
// ref recurses, a generator is silence. Visibility never gates audio.
std::vector<NodeLink> effective_links(const Look& look) {
    return look.links.empty() ? synthesize_links(look) : look.links;
}

uint64_t link_into(const std::vector<NodeLink>& links, uint64_t to,
                   uint32_t port) {
    for (const NodeLink& l : links)
        if (l.to == to && l.to_port == port) return l.from;
    return 0;
}

// Walks back from `start` (inclusive) through port-0 inputs to the
// chain's source layer, appending live audio hops to `rev` in
// output-first order (capped at kMaxVoiceOps; the hops nearest the
// output win). Null root = generator-free dead end (dangling/unwired).
// audio_off accumulates live audio-targeting Offset shims sitting
// DIRECTLY on the source layer (the only position where they apply).
void walk_chain(const Look& look, const std::vector<NodeLink>& links,
                uint64_t start, const Layer** root,
                std::vector<AudioOp>& rev, int64_t* audio_off) {
    auto find_layer = [&](uint64_t id) -> const Layer* {
        for (const Layer& l : look.layers)
            if (l.id == id) return &l;
        return nullptr;
    };
    auto find_fx = [&](uint64_t id,
                       const Layer** owner) -> const EffectInstance* {
        for (const Layer& l : look.layers)
            for (const EffectInstance& fx : l.stack)
                if (fx.id == id) {
                    *owner = &l;
                    return &fx;
                }
        return nullptr;
    };
    auto group_bypassed = [&](const Layer& l, uint64_t gid) {
        if (gid == 0) return false;
        for (const Group& g : l.groups)
            if (g.id == gid) return g.bypass;
        return false;
    };
    uint64_t cur = start;
    for (int guard = 0; guard < 512 && cur; ++guard) {
        if (const Layer* layer = find_layer(cur)) {
            *root = layer;
            return;
        }
        const Layer* owner = nullptr;
        const EffectInstance* fx = find_fx(cur, &owner);
        if (!fx) return;   // dangling id: silent
        if (is_audio_effect(fx->type) && !fx->bypass &&
            !group_bypassed(*owner, fx->group_id) &&
            rev.size() < kMaxVoiceOps) {
            AudioOp op;
            op.type = fx->type;
            const size_t n = std::min<size_t>(fx->params.size(), 4);
            for (size_t i = 0; i < n; ++i) op.params[i] = fx->params[i];
            op.wet = fx->wet;
            rev.push_back(op);
        }
        const uint64_t next = link_into(links, fx->id, 0);
        if (audio_off && fx->type == EffectType::Offset && !fx->bypass &&
            !group_bypassed(*owner, fx->group_id) &&
            offset_targets_audio(*fx) && find_layer(next))
            *audio_off += offset_frames(*fx);
        cur = next;
    }
}

struct Voice {
    const Layer* root = nullptr;   // null = silent look
    std::vector<AudioOp> ops;
    int64_t audio_off = 0;         // Offset shims on the voice's source
};

Voice resolve_voice(const Look& look) {
    Voice v;
    const std::vector<NodeLink> links = effective_links(look);
    std::vector<AudioOp> rev;   // collected output-first
    walk_chain(look, links,
               link_into(links, 0, look.audio_split ? 1u : 0u), &v.root,
               rev, &v.audio_off);
    v.ops.assign(rev.rbegin(), rev.rend());
    return v;
}

// A look's sources run in LOCKSTEP: identity clock, windowed only by
// what the source can play. Media layers add their slip; nested entities pass
// time straight through, cut by an explicit duration when one is set.
// The picture walk emits every visible wired-or-not layer (compile
// culls by wiring itself); the audio walk emits only the voice.
void walk_look(const Document& doc, const Look& look, const Cursor& cur,
               bool audio, std::vector<MediaInstance>& out) {
    auto emit_media = [&](const Layer& layer,
                          const std::vector<AudioOp>& chain, int64_t off) {
        if (!layer.asset) return;
        const Asset* a = doc.find_asset(layer.asset);
        // An asset with no picture (audio import without cover art:
        // no frames, no dimensions) has no image side: the PICTURE walk
        // emits nothing - the compiler mirrors this - while the audio
        // walk carries the voice.
        if (!audio && a && !a->frame_count && !a->width && !a->height)
            return;
        const uint32_t frames = a ? a->frame_count : 0;
        // Playable window: media = local + slip + off must stay inside
        // the asset; a negative shift delays the start (closed gate
        // before it), a positive one shortens the tail.
        const int64_t shift = static_cast<int64_t>(layer.slip) + off;
        const double lo = shift < 0 ? static_cast<double>(-shift) : 0.0;
        double hi = kUnbounded;
        if (frames) {
            hi = static_cast<double>(frames) - static_cast<double>(shift);
            if (hi < lo) hi = lo;
        }
        Cursor leaf;
        if (layer.timeline_lock) {
            // The node reads the asset at the ROOT clock: identity map,
            // ignoring every composed placement hop. The window bounds
            // are the same formulas read in root frames.
            leaf.depth = cur.depth + 1;
            leaf.a = 1.0;
            leaf.b = static_cast<double>(shift);
            leaf.r0 = std::max(cur.r0, lo);
            leaf.r1 = std::min(cur.r1, hi);
            if (leaf.r1 <= leaf.r0) return;
        } else if (!child_window(cur, lo, hi, 1.0,
                                 lo + static_cast<double>(shift), &leaf)) {
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
        c.gain = cur.gain;
        // This voice's own hops first, then every enclosing one.
        for (const AudioOp& op : chain)
            if (c.op_count < kMaxVoiceOps) c.ops[c.op_count++] = op;
        for (const AudioOp& op : cur.ops)
            if (c.op_count < kMaxVoiceOps) c.ops[c.op_count++] = op;
        out.push_back(c);
    };
    auto descend_nested = [&](const Layer& layer,
                              const std::vector<AudioOp>& chain,
                              int64_t off) {
        if (!layer.target) return;
        if (cur.depth + 1 >= kMaxLookDepth) return;
        // An explicit duration cuts the nested entity; a derived one
        // equals its content bounds, so only the explicit case clamps.
        // A shift moves the child clock: child local = local + off.
        double dur = 0.0;
        if (const Look* t = doc.find_look(layer.target)) {
            dur = static_cast<double>(t->duration);
        } else if (const Sequence* t = doc.find_sequence(layer.target)) {
            dur = static_cast<double>(t->duration);
        } else {
            return;   // dangling ref: dormant
        }
        const double lo = off < 0 ? static_cast<double>(-off) : 0.0;
        double hi = kUnbounded;
        if (dur > 0.0) {
            hi = dur - static_cast<double>(off);
            if (hi < lo) hi = lo;
        }
        Cursor child;
        if (!child_window(cur, lo, hi, 1.0,
                          lo + static_cast<double>(off), &child))
            return;
        child.entity = layer.target;
        child.path = nested_child_path(cur.path, layer.id, layer.target,
                                       off);
        child.gain = cur.gain;
        child.ops = chain;
        child.ops.insert(child.ops.end(), cur.ops.begin(), cur.ops.end());
        walk(doc, child, audio, out);
    };

    if (audio) {
        if (out.size() >= kMaxFlattened) return;
        const Voice voice = resolve_voice(look);
        if (!voice.root) return;
        if (layer_is_media(*voice.root))
            emit_media(*voice.root, voice.ops, voice.audio_off);
        else if (layer_is_nested(*voice.root))
            descend_nested(*voice.root, voice.ops, voice.audio_off);
        return;
    }
    // Live video-targeting Offset shims sitting DIRECTLY on a source
    // (port-0 input is the layer): each adds a shifted read of that
    // source next to the base one. Liveness mirrors compile's
    // effect_active exactly - owner layer visible, not bypassed, not
    // muted by a solo elsewhere in its layer, group live.
    std::vector<std::pair<uint64_t, int64_t>> vshifts;
    {
        const std::vector<NodeLink> links = effective_links(look);
        for (const Layer& holder : look.layers) {
            if (!holder.visible) continue;
            bool any_solo = false;
            for (const EffectInstance& fx : holder.stack)
                if (fx.solo && !fx.bypass) any_solo = true;
            auto group_off = [&](uint64_t gid) {
                if (!gid) return false;
                for (const Group& g : holder.groups)
                    if (g.id == gid) return g.bypass;
                return false;
            };
            for (const EffectInstance& fx : holder.stack) {
                if (fx.type != EffectType::Offset || fx.bypass) continue;
                if (any_solo && !fx.solo) continue;
                if (group_off(fx.group_id)) continue;
                if (!offset_targets_video(fx)) continue;
                const int64_t off = offset_frames(fx);
                if (!off) continue;
                const uint64_t src = link_into(links, fx.id, 0);
                for (const Layer& l : look.layers)
                    if (l.id == src) vshifts.emplace_back(src, off);
            }
        }
    }
    for (const Layer& layer : look.layers) {
        if (!layer.visible) continue;
        if (out.size() >= kMaxFlattened) return;
        if (layer_is_media(layer)) {
            emit_media(layer, {}, 0);
        } else if (layer_is_nested(layer)) {
            descend_nested(layer, {}, 0);
        } else {
            continue;
        }
        for (const auto& [src, off] : vshifts) {
            if (src != layer.id) continue;
            if (out.size() >= kMaxFlattened) return;
            if (layer_is_media(layer))
                emit_media(layer, {}, off);
            else
                descend_nested(layer, {}, off);
        }
    }
}

// A sequence's placements are the affine hops. The picture walk reads
// the video lanes; the audio walk reads the audio tracks - video lanes
// are silent, sound rides audio placements only.
void walk_sequence(const Document& doc, const Sequence& seq,
                   const Cursor& cur, bool audio,
                   std::vector<MediaInstance>& out) {
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
        child.ops = cur.ops;   // sequences arrange; DSP passes through
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
          std::vector<MediaInstance>& out) {
    if (const Look* look = doc.find_look(cur.entity)) {
        walk_look(doc, *look, cur, audio, out);
        return;
    }
    if (const Sequence* seq = doc.find_sequence(cur.entity))
        walk_sequence(doc, *seq, cur, audio, out);
}

std::vector<MediaInstance> flatten(const Document& doc, uint64_t root_id,
                                  bool audio) {
    std::vector<MediaInstance> out;
    Cursor root;
    root.entity = root_id;
    root.path = root_id;   // the root instance's path is its own id
    walk(doc, root, audio, out);
    return out;
}

}  // namespace

std::vector<MediaInstance> flatten_media_sources(const Document& doc,
                                               uint64_t root_id) {
    return flatten(doc, root_id, /*audio=*/false);
}

std::vector<MediaInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id) {
    return flatten(doc, root_id, /*audio=*/true);
}

AudioChain resolve_audio_chain(const Document& doc, const Look& look,
                               uint64_t node) {
    AudioChain out;
    std::vector<AudioOp> rev;   // output-first across every nesting hop
    int64_t off = 0;            // Offset shims sum across the hops
    const Look* cur = &look;
    uint64_t start = node;
    for (int depth = 0; depth < kMaxLookDepth; ++depth) {
        if (!start) return {};
        const std::vector<NodeLink> links = effective_links(*cur);
        const Layer* root = nullptr;
        walk_chain(*cur, links, start, &root, rev, &off);
        if (!root) return {};
        if (layer_is_media(*root)) {
            if (!root->asset) return {};
            out.asset = root->asset;
            out.slip = root->slip;
            out.offset = off;
            out.locked = root->timeline_lock;
            out.op_count = static_cast<uint32_t>(
                std::min(rev.size(), kMaxVoiceOps));
            // Reversing output-first gives play order: the deepest
            // nesting level's hops run first.
            for (uint32_t i = 0; i < out.op_count; ++i)
                out.ops[i] = rev[rev.size() - 1 - i];
            return out;
        }
        if (!layer_is_nested(*root) || !root->target) return {};
        const Look* t = doc.find_look(root->target);
        if (!t) return {};   // sequence ref: no single voice
        const std::vector<NodeLink> tlinks = effective_links(*t);
        start = link_into(tlinks, 0, t->audio_split ? 1u : 0u);
        cur = t;
    }
    return {};
}

}  // namespace looks::doc
