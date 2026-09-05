#include "doc/instances.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "util/hash.h"

namespace looks::doc {

namespace {

// The depth guard alone allows kMaxLayers^kMaxLookDepth: cap the walk.
constexpr size_t kMaxFlattened = 1024;

struct Cursor {
    uint64_t entity = 0;   // a look or a sequence
    uint64_t path = 0;
    int depth = 0;
    // local = a * root + b maps root frames onto this entity's own clock.
    double a = 1.0, b = 0.0;
    // [r0, r1) is the root span this instance plays on.
    double r0 = 0.0, r1 = kUnbounded;
    // Root entity rate: timeline-locked media conforms against this.
    double root_fps = 30.0;
};

// The bounds are real, not whole: the compiler point test must agree here.
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

// Link-vector order is stacking order: this returns the bottom live feed.
uint64_t link_into(const Look& look, const std::vector<NodeLink>& links,
                   uint64_t to, uint32_t port) {
    for (const NodeLink& l : links)
        if (l.to == to && l.to_port == port &&
            wire_producer_live(look, l.from))
            return l.from;
    return 0;
}

// ops are in play order, source first.
struct VoicePath {
    const Layer* root = nullptr;
    std::vector<AudioOp> ops;
    int64_t audio_off = 0;   // Offset shims sitting on the path's source
};

// The caps bound the walk through the one legal cycle, Feedback.
constexpr size_t kMaxVoicePaths = 64;
constexpr int kVoiceVisitBudget = 4096;
constexpr int kMaxVoiceDepth = 256;

// Paths emit bottom-first: paths.front() is the bottom-most chain.
// On overflow the hops nearest the output win.
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
        // Adjacency looks through slots; the shim rides source branches only.
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

// port 0 = the combined In, port 1 = the split audio-in. Unwired = silent.
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

// One memo per instance: every nesting hop composes its own clock.
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

bool media_leaf(const Document& doc, const Layer& layer, const Asset& a,
                const Cursor& cur, double eff, int64_t off, Cursor* leaf,
                double* rate, int64_t* shift) {
    *shift = static_cast<int64_t>(layer.slip) + off;
    *rate = media_conform_rate(doc, a,
                               layer.timeline_lock ? cur.root_fps : eff);
    double lo = 0.0, hi = 0.0;
    shifted_window(static_cast<double>(a.frame_count), *shift, *rate, &lo,
                   &hi);
    if (layer.timeline_lock) {
        // Locked: read at the root clock, ignore every placement hop.
        leaf->depth = cur.depth + 1;
        leaf->a = 1.0;
        leaf->b = 0.0;
        leaf->r0 = std::max(cur.r0, lo);
        leaf->r1 = std::min(cur.r1, hi);
        return leaf->r1 > leaf->r0;
    }
    return child_window(cur, lo, hi, 1.0, lo, leaf);
}

bool nested_child(const Document& doc, const Layer& layer, const Cursor& cur,
                  double eff, int64_t off, Cursor* child) {
    if (!layer.target) return false;
    if (cur.depth + 1 >= kMaxLookDepth) return false;
    // Only an explicit duration clamps: a derived one is the content.
    double dur = 0.0;
    if (const Look* t = doc.find_look(layer.target)) {
        dur = static_cast<double>(t->duration);
    } else if (const Sequence* t = doc.find_sequence(layer.target)) {
        dur = static_cast<double>(t->duration);
    } else {
        return false;   // dangling ref: dormant
    }
    const double ratio = entity_fps(doc, layer.target) / eff;
    double lo = 0.0, hi = 0.0;
    shifted_window(dur, off, ratio, &lo, &hi);
    if (!child_window(cur, lo, hi, ratio,
                      lo * ratio + static_cast<double>(off), child))
        return false;
    child->entity = layer.target;
    child->path = nested_child_path(cur.path, layer.id, layer.target, off);
    child->root_fps = cur.root_fps;
    return true;
}

bool placement_child(const Document& doc, uint64_t track_id,
                     const Placement& p, const Cursor& cur, double eff,
                     Cursor* child) {
    if (!p.target) return false;
    if (cur.depth + 1 >= kMaxLookDepth) return false;
    if (!doc.find_look(p.target) && !doc.find_sequence(p.target))
        return false;   // dangling target: dormant
    const double ratio = placement_ratio(doc, p, eff);
    const double speed =
        p.speed > 0.0f ? static_cast<double>(p.speed) * ratio : 0.0;
    const uint32_t len = source_length(doc, p);
    const uint32_t end = placement_end(p, len, ratio);
    const double lo = static_cast<double>(p.t_in);
    const double hi = end ? static_cast<double>(end) : kUnbounded;
    if (!child_window(cur, lo, hi, speed, static_cast<double>(p.source_in),
                      child))
        return false;
    child->entity = p.target;
    child->path = seq_child_path(cur.path, track_id, p.target);
    child->root_fps = cur.root_fps;
    return true;
}

int sum_or_pass(ProgBuild& pb, std::vector<int>& children) {
    if (children.empty()) return -1;
    if (children.size() == 1) return children[0];
    const int idx = alloc_audio_node(pb);
    if (idx < 0) return -1;
    pb.prog.nodes[static_cast<size_t>(idx)].inputs = std::move(children);
    return idx;
}

std::vector<int> collect_feeds(LookBuild& lb, uint64_t to, uint32_t port,
                               int64_t shift) {
    std::vector<int> children;
    for (const NodeLink& l : lb.links) {
        if (l.to != to || l.to_port != port ||
            !wire_producer_live(lb.look, l.from))
            continue;
        const int c = build_doc_node(lb, l.from, shift);
        if (c >= 0) children.push_back(c);
    }
    return children;
}

int build_layer_audio(LookBuild& lb, const Layer& layer,
                      int64_t src_shift) {
    const Document& doc = lb.pb.doc;
    const Cursor& cur = lb.cur;
    if (layer_is_media(layer)) {
        if (!layer.asset) return -1;
        const Asset* a = doc.find_asset(layer.asset);
        if (!a) return -1;   // dangling id: dormant
        Cursor leaf;
        double rate = 0.0;
        int64_t shift = 0;
        if (!media_leaf(doc, layer, *a, cur, lb.eff, src_shift, &leaf,
                        &rate, &shift))
            return -1;
        const int idx = alloc_audio_node(lb.pb);
        if (idx < 0) return -1;
        AudioNode& n = lb.pb.prog.nodes[static_cast<size_t>(idx)];
        n.asset = layer.asset;
        // Only the Offset shim folds into the key, never the slip.
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
        n.w0 = leaf.r0;
        n.w1 = leaf.r1;
        return idx;
    }
    if (layer_is_nested(layer)) {
        Cursor child;
        if (!nested_child(doc, layer, cur, lb.eff, src_shift, &child))
            return -1;
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

// -1 = silence: dangling, unfed, generator, or the Feedback back edge.
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
            // Offset shifts only the sources it sits directly on.
            int64_t feed_shift = 0;
            if (fx->type == EffectType::Offset && live &&
                offset_targets_audio(*fx))
                feed_shift = offset_frames(*fx);
            std::vector<int> children = collect_feeds(lb, id, 0, feed_shift);
            if (!children.empty() && is_audio_effect(fx->type) && live) {
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
            } else {
                result = sum_or_pass(lb.pb, children);
            }
        } else if (group_of_input(lb.look, id)) {
            std::vector<int> children = collect_feeds(lb, id, 0, src_shift);
            result = sum_or_pass(lb.pb, children);
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
    std::vector<int> children =
        collect_feeds(lb, 0, look.audio_split ? 1u : 0u, 0);
    return sum_or_pass(pb, children);
}

int build_seq_audio(ProgBuild& pb, const Sequence& seq,
                    const Cursor& cur) {
    const double eff = effective_fps(pb.doc, seq);
    std::vector<int> children;
    for (const AudioTrack& t : seq.audio) {
        const float track_gain = t.mute ? 0.0f : std::max(t.gain, 0.0f);
        if (track_gain <= 0.0f) continue;
        for (const Placement& p : t.placements) {
            const float pgain =
                p.audio_mute
                    ? 0.0f
                    : track_gain * std::max(p.audio_gain, 0.0f);
            if (pgain <= 0.0f) continue;
            Cursor child;
            if (!placement_child(pb.doc, t.id, p, cur, eff, &child))
                continue;
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
    return sum_or_pass(pb, children);
}

int build_entity_audio(ProgBuild& pb, uint64_t entity, const Cursor& cur) {
    if (const Look* look = pb.doc.find_look(entity))
        return build_look_audio(pb, *look, cur);
    if (const Sequence* seq = pb.doc.find_sequence(entity))
        return build_seq_audio(pb, *seq, cur);
    return -1;
}

// Emits every visible layer, wired or not: compile culls by wiring.
void walk_look(const Document& doc, const Look& look, const Cursor& cur,
               std::vector<MediaInstance>& out) {
    const double eff = effective_fps(doc, look);
    auto emit_media = [&](const Layer& layer, int64_t off) {
        if (!layer.asset) return;
        const Asset* a = doc.find_asset(layer.asset);
        // No frames and no size means audio only: there is no image side.
        if (!a) return;
        if (!a->frame_count && !a->width && !a->height) return;
        Cursor leaf;
        double rate = 0.0;
        int64_t shift = 0;
        if (!media_leaf(doc, layer, *a, cur, eff, off, &leaf, &rate, &shift))
            return;
        MediaInstance c;
        // The key must match the compiler Source stamp.
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
        Cursor child;
        if (nested_child(doc, layer, cur, eff, off, &child))
            walk(doc, child, out);
    };

    // Liveness must mirror compile effect_active exactly.
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
                // Adjacency looks through group input slots, like the compiler.
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

void walk_sequence(const Document& doc, const Sequence& seq,
                   const Cursor& cur, std::vector<MediaInstance>& out) {
    const double eff = effective_fps(doc, seq);
    // The flatten and the compiler must agree on hidden lanes.
    for (const SeqTrack& t : seq.tracks) {
        if (t.hidden) continue;
        for (const Placement& p : t.placements) {
            if (out.size() >= kMaxFlattened) return;
            Cursor child;
            if (placement_child(doc, t.id, p, cur, eff, &child))
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

namespace {

bool entity_has_image_at(const Document& doc, uint64_t id, int depth);

// A layer draws when its own source can make pixels.
bool layer_draws(const Document& doc, const Layer& l, int depth) {
    if (!l.visible) return false;
    if (layer_is_media(l)) {
        const Asset* a = l.asset ? doc.find_asset(l.asset) : nullptr;
        return a && (a->frame_count || a->width || a->height);
    }
    if (layer_is_nested(l))
        return l.target && entity_has_image_at(doc, l.target, depth + 1);
    return true;   // a generator always draws
}

// Walks back from the Output's video port, so a look whose image side is
// unwired reports no image even when it holds drawable layers.
bool feed_draws(const Document& doc, const Look& look,
                const std::vector<NodeLink>& links, uint64_t id,
                std::vector<uint64_t>& seen, int depth) {
    if (depth >= kMaxLookDepth * 8) return false;
    for (uint64_t s : seen)
        if (s == id) return false;
    seen.push_back(id);
    for (const Layer& l : look.layers)
        if (l.id == id) return layer_draws(doc, l, depth);
    for (const NodeLink& l : links)
        if (l.to == id && l.to_port == 0 &&
            feed_draws(doc, look, links, l.from, seen, depth + 1))
            return true;
    return false;
}

bool entity_has_image_at(const Document& doc, uint64_t id, int depth) {
    if (depth >= kMaxLookDepth) return false;
    if (const Look* look = doc.find_look(id)) {
        std::vector<NodeLink> synth;
        const std::vector<NodeLink>& links = effective_links(*look, synth);
        for (const NodeLink& l : links) {
            if (l.to != 0 || l.to_port != 0) continue;
            std::vector<uint64_t> seen;
            if (feed_draws(doc, *look, links, l.from, seen, depth))
                return true;
        }
        return false;
    }
    if (const Sequence* seq = doc.find_sequence(id)) {
        for (const SeqTrack& t : seq->tracks) {
            if (t.hidden) continue;
            for (const Placement& p : t.placements)
                if (p.target && entity_has_image_at(doc, p.target, depth + 1))
                    return true;
        }
    }
    return false;
}

}  // namespace

bool entity_has_image(const Document& doc, uint64_t root_id) {
    return entity_has_image_at(doc, root_id, 0);
}

std::vector<MediaInstance> flatten_audio_sources(const Document& doc,
                                                uint64_t root_id) {
    const AudioProgram prog = flatten_audio_program(doc, root_id);
    std::vector<MediaInstance> out;
    if (prog.root < 0) return out;
    // Depth-first in stored order: a node reached twice emits once.
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
    // Analysis needs one stream: take the bottom-most resolvable path.
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
            // Unbound or dangling: no voice.
            if (!root.asset || !doc.find_asset(root.asset)) return {};
            out.asset = root.asset;
            out.slip = root.slip;
            out.offset = off;
            // Lockstep hops are 1:1 in time: the rate uses the start clock.
            out.rate = media_conform_rate(doc, *doc.find_asset(root.asset),
                                          effective_fps(doc, look));
            out.locked = root.timeline_lock;
            out.op_count = static_cast<uint32_t>(
                std::min(chain.size(), kMaxVoiceOps));
            // On overflow the hops nearest the walk start win.
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
