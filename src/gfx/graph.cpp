#include "gfx/graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "doc/instances.h"
#include "util/hash.h"

namespace looks::gfx {

namespace {

// The graph holds one shared motion-vector field per frame.
struct FlowSlot {
    int node = -1;
};

// Ownership maps stay local to emit_look; two hops must never share entries.
struct Compiler {
    const doc::Document& doc;
    RenderGraph& graph;
    uint64_t preview_node = 0;
    uint64_t preview_layer = 0;
    uint64_t measure_placement = 0;
    // The before pass bypasses effects but keeps the composition whole.
    bool strip_effects = false;
    // Root frame rate; timeline-locked media conforms to it.
    double root_fps = 30.0;
    FlowSlot flow;
    int zero_node = -1;

    int add(GraphNode node, int instance, uint64_t key = 0) {
        node.instance = instance;
        node.key = key;
        graph.nodes.push_back(std::move(node));
        return static_cast<int>(graph.nodes.size()) - 1;
    }

    // Unwired nodes stay dormant; never feed them a fabricated frame.
    int zero() {
        if (zero_node < 0) {
            GraphNode b;
            b.kind = GraphNode::Kind::Generator;   // layer -1 = zero
            zero_node = add(std::move(b), 0);
        }
        return zero_node;
    }

    // Multi-pass expansion order must match the engine's pass_index dispatch.
    int emit_effect(const doc::EffectInstance& fx, int layer_index,
                    int stack_index, int upstream, int instance, uint64_t key,
                    int extra_input) {
        auto make = [&](int pass, std::vector<int> inputs) {
            GraphNode n;
            n.kind = GraphNode::Kind::Effect;
            n.layer_index = layer_index;
            n.effect_index = stack_index;
            n.pass_index = pass;
            n.inputs = std::move(inputs);
            return n;
        };

        if (fx.type == doc::EffectType::Glow) {
            const int bright = add(make(0, {upstream}), instance, key);
            const int blur_h = add(make(1, {bright}), instance, key);
            const int blur_v = add(make(2, {blur_h}), instance, key);
            return add(make(3, {upstream, blur_v}), instance, key);
        }
        if (fx.type == doc::EffectType::FlowSmear ||
            fx.type == doc::EffectType::Datamosh ||
            fx.type == doc::EffectType::FlowParticles ||
            fx.type == doc::EffectType::FlowPaint ||
            (fx.type == doc::EffectType::Quantize && fx.params.size() > 8 &&
             fx.params[8] >= 0.5f) ||
            (fx.type == doc::EffectType::Dither && fx.params.size() > 7 &&
             fx.params[7] >= 0.5f)) {
            // Quantize/Dither need flow only in motion-locked mode.
            if (flow.node < 0) {
                GraphNode f;
                f.kind = GraphNode::Kind::Flow;
                flow.node = add(std::move(f), 0);
            }
            return add(make(0, {upstream, flow.node}), instance, key);
        }
        if (doc::effect_aux_port(fx.type) && extra_input >= 0)
            return add(make(0, {upstream, extra_input}), instance, key);
        return add(make(0, {upstream}), instance, key);
    }

    // Must stay identical to the flatten and click-pick predicates.
    const doc::Placement* winner_at(
        const std::vector<doc::Placement>& placements,
        const LookInstance& p, double parent_fps) const {
        return doc::placement_winner(doc, placements, p.local_time,
                                     parent_fps);
    }

    int emit_entity(uint64_t id, int inst, bool is_root) {
        if (doc.find_look(id)) return emit_look(id, inst, is_root);
        if (doc.find_sequence(id)) return emit_sequence(id, inst, is_root);
        return -1;
    }

    int emit_look(uint64_t look_id, int inst, bool is_root);
    int emit_sequence(uint64_t seq_id, int inst, bool is_root);
};

int Compiler::emit_sequence(uint64_t seq_id, int inst, bool is_root) {
    const doc::Sequence& seq = doc.sequence(seq_id);
    const double eff = doc::effective_fps(doc, seq);
    int below = -1;
    for (const doc::SeqTrack& track : seq.tracks) {
        if (track.hidden) continue;
        // Refetch each lane: recursion appends instances and may reallocate.
        const LookInstance self =
            graph.instances[static_cast<size_t>(inst)];
        if (self.depth + 1 >= doc::kMaxLookDepth) break;
        const doc::Placement* place =
            winner_at(track.placements, self, eff);
        if (!place || !place->target) continue;
        if (!doc.find_look(place->target) &&
            !doc.find_sequence(place->target))
            continue;  // dangling block: dormant
        LookInstance child;
        child.look = place->target;
        child.path =
            doc::seq_child_path(self.path, track.id, place->target);
        child.depth = self.depth + 1;
        // Evaluate on continuous parent time; floor only the effect frame.
        child.local_time = doc::placement_source_frame(
            *place, self.local_time,
            doc::placement_ratio(doc, *place, eff));
        child.local_frame = child.local_time <= 0.0
            ? 0u
            : static_cast<uint32_t>(std::floor(child.local_time));
        graph.instances.push_back(child);
        const int ci = static_cast<int>(graph.instances.size()) - 1;
        const int out = emit_entity(place->target, ci, /*is_root=*/false);
        if (out < 0) continue;
        // Pre-Motion by design: the monitor's box math applies the transform.
        if (is_root && measure_placement && place->id == measure_placement)
            graph.measure = out;
        // A bottom lane with Motion or opacity blends over transparent black.
        const float popa = std::clamp(place->opacity, 0.0f, 1.0f);
        const bool moved = doc::placement_has_transform(*place);
        if (below < 0 && popa >= 1.0f && !moved) {
            below = out;
        } else {
            GraphNode blend;
            blend.kind = GraphNode::Kind::LayerBlend;
            blend.layer_index = -1;
            blend.p_opacity = popa;
            if (moved) {
                blend.p_shift_x = place->pos_x;
                blend.p_shift_y = place->pos_y;
                blend.p_scale = place->scale;
                blend.p_rotate = place->rotate * doc::kDeg2Rad;
                blend.p_anchor_x = place->anchor_x;
                blend.p_anchor_y = place->anchor_y;
            }
            blend.inputs = {below < 0 ? zero() : below, out};
            below = add(std::move(blend), inst);
        }
    }
    return below;
}

int Compiler::emit_look(uint64_t look_id, int inst, bool is_root) {
    const doc::Look& look = doc.look(look_id);
    const double eff = doc::effective_fps(doc, look);
    // By value: recursion appends instances, which may reallocate.
    const uint64_t path = graph.instances[static_cast<size_t>(inst)].path;
    auto subject_key = [&](uint64_t id) { return hash_combine(path, id); };

    // An empty link table compiles through a synthesized stack-order chain.
    std::vector<doc::NodeLink> links_synth;
    const std::vector<doc::NodeLink>& links =
        doc::effective_links(look, links_synth);
    auto link_into = [&](uint64_t to, uint32_t port) -> uint64_t {
        for (const doc::NodeLink& l : links)
            if (l.to == to && l.to_port == port) return l.from;
        return 0;
    };

    // Solo mutes only within the owner layer.
    std::unordered_map<uint64_t, size_t> owner;    // fx id to layer index
    std::unordered_map<uint64_t, const doc::EffectInstance*> fx_by_id;
    std::vector<char> layer_solo(look.layers.size(), 0);
    for (size_t li = 0; li < look.layers.size(); ++li)
        for (const doc::EffectInstance& fx : look.layers[li].stack) {
            owner[fx.id] = li;
            fx_by_id[fx.id] = &fx;
            if (fx.solo && !fx.bypass) layer_solo[li] = 1;
        }
    auto group_bypassed_in = [&](size_t li, uint64_t gid) {
        if (gid == 0) return false;
        for (const doc::Group& g : look.layers[li].groups)
            if (g.id == gid) return g.bypass;
        return false;
    };
    auto effect_active = [&](uint64_t id) {
        if (strip_effects) return false;
        const auto ito = owner.find(id);
        if (ito == owner.end()) return false;
        const size_t li = ito->second;
        const doc::EffectInstance& fx = *fx_by_id[id];
        // Audio effects are image-identity; the image graph routes past them.
        // Offset never dispatches; pass A2 turns adjacent ones into shims.
        if (doc::is_audio_effect(fx.type) ||
            fx.type == doc::EffectType::Offset)
            return false;
        return look.layers[li].visible && !fx.bypass &&
               !(layer_solo[li] && !fx.solo) &&
               !group_bypassed_in(li, fx.group_id);
    };

    // A later call replaces an earlier one, so a wrapped card taps its wrap.
    auto set_tap = [&](uint64_t card, int node) {
        if (!is_root || node < 0) return;
        for (auto& t : graph.thumb_taps)
            if (t.first == card) {
                t.second = node;
                return;
            }
        graph.thumb_taps.emplace_back(card, node);
    };

    // Pass A: layer source heads, all in lockstep with this look's clock.
    // A wire to a time-culled producer is a closed gate, never unwired.
    std::unordered_map<uint64_t, int> heads;   // layer id to node index
    std::unordered_set<uint64_t> head_ungated;   // matte wired, not yet gated
    std::unordered_map<uint64_t, char> time_culled;
    const LookInstance self = graph.instances[static_cast<size_t>(inst)];
    for (size_t li = 0; li < look.layers.size(); ++li) {
        const doc::Layer& layer = look.layers[li];
        if (!layer.visible) continue;
        int cur = -1;
        if (doc::layer_is_media(layer)) {
            // An unbound media node is dormant, like an unwired port.
            // A timeline-locked node reads and windows on the root clock.
            if (!layer.asset) continue;
            const doc::Asset* a = doc.find_asset(layer.asset);
            // An audio-only asset gets no image head; the audio walk uses it.
            if (!a) continue;
            if (!a->frame_count && !a->width && !a->height) continue;
            const uint32_t frames = a->frame_count;
            const double t = layer.timeline_lock
                                 ? graph.instances[0].local_time
                                 : self.local_time;
            {
                double lo = 0.0, hi = 0.0;
                doc::shifted_window(
                    static_cast<double>(frames),
                    static_cast<int64_t>(layer.slip),
                    doc::media_conform_rate(
                        doc, *a, layer.timeline_lock ? root_fps : eff),
                    &lo, &hi);
                if (t < lo || t >= hi) {
                    time_culled[layer.id] = 1;
                    continue;
                }
            }
            GraphNode src;
            src.kind = GraphNode::Kind::Source;
            src.layer_index = static_cast<int>(li);
            cur = add(std::move(src), inst,
                      doc::media_stream_key(path, layer.id, layer.asset,
                                            layer.timeline_lock, 0));
            if (graph.source < 0) graph.source = cur;
        } else if (doc::layer_is_nested(layer)) {
            // A nested entity plays 1:1 with this clock; its duration cuts it.
            if (!layer.target) continue;
            const doc::Look* tl = doc.find_look(layer.target);
            const doc::Sequence* ts =
                tl ? nullptr : doc.find_sequence(layer.target);
            if (!tl && !ts) continue;
            const uint32_t dur = tl ? tl->duration : ts->duration;
            const double ratio = doc::entity_fps(doc, layer.target) / eff;
            {
                double lo = 0.0, hi = 0.0;
                doc::shifted_window(static_cast<double>(dur), 0, ratio,
                                    &lo, &hi);
                if (self.local_time < lo || self.local_time >= hi) {
                    time_culled[layer.id] = 1;
                    continue;
                }
            }
            if (self.depth + 1 >= doc::kMaxLookDepth) continue;
            LookInstance child;
            child.look = layer.target;
            child.path = doc::nested_child_path(path, layer.id,
                                                layer.target, 0);
            child.depth = self.depth + 1;
            // Lockstep is 1:1 in time; the hop ratio scales the child clock.
            child.local_time = self.local_time * ratio;
            child.local_frame = child.local_time <= 0.0
                ? 0u
                : static_cast<uint32_t>(std::floor(child.local_time));
            graph.instances.push_back(child);
            const int ci = static_cast<int>(graph.instances.size()) - 1;
            const int out =
                emit_entity(layer.target, ci, /*is_root=*/false);
            if (out < 0) continue;
            cur = out;
        } else {
            // Generators have no media and no end: always on.
            GraphNode gen;
            gen.kind = GraphNode::Kind::Generator;
            gen.layer_index = static_cast<int>(li);
            cur = add(std::move(gen), inst, subject_key(layer.id));
        }
        if (doc::layer_has_transform(layer)) {
            GraphNode xf;
            xf.kind = GraphNode::Kind::LayerTransform;
            xf.layer_index = static_cast<int>(li);
            xf.inputs.push_back(cur);
            cur = add(std::move(xf), inst, subject_key(layer.id));
        }
        heads[layer.id] = cur;
        set_tap(layer.id | kThumbSourceBit, cur);
        if (link_into(layer.id, 1)) head_ungated.insert(layer.id);
    }

    std::unordered_map<uint64_t, int> fx_out;   // fx id to output node

    // Pass A2: an Offset wired directly on a source becomes a shifted read
    // and re-keys its stream; wired anywhere else it passes through.
    std::unordered_map<uint64_t, char> offset_terminal;
    if (!strip_effects) {
        for (size_t li = 0; li < look.layers.size(); ++li) {
            if (!look.layers[li].visible) continue;
            for (const doc::EffectInstance& fx : look.layers[li].stack) {
                if (fx.type != doc::EffectType::Offset || fx.bypass)
                    continue;
                if (layer_solo[li] && !fx.solo) continue;
                if (group_bypassed_in(li, fx.group_id)) continue;
                if (!doc::offset_targets_video(fx)) continue;
                const int64_t off = doc::offset_frames(fx);
                if (!off) continue;
                // Adjacency looks through group input slots.
                const uint64_t src =
                    doc::hop_group_inputs(look, links,
                                          link_into(fx.id, 0));
                const doc::Layer* sl = nullptr;
                size_t sli = 0;
                for (size_t k = 0; k < look.layers.size(); ++k)
                    if (look.layers[k].id == src) {
                        sl = &look.layers[k];
                        sli = k;
                    }
                if (!sl || !sl->visible ||
                    !(doc::layer_is_media(*sl) || doc::layer_is_nested(*sl)))
                    continue;
                offset_terminal[fx.id] = 1;
                int shifted = -1;
                if (doc::layer_is_media(*sl)) {
                    if (!sl->asset) continue;   // dormant, like the base
                    const doc::Asset* a = doc.find_asset(sl->asset);
                    // Dangling or audio-only: image-dormant, like the base.
                    if (!a || (!a->frame_count && !a->width && !a->height))
                        continue;
                    const uint32_t frames = a->frame_count;
                    const double t = sl->timeline_lock
                                         ? graph.instances[0].local_time
                                         : self.local_time;
                    double lo = 0.0, hi = 0.0;
                    doc::shifted_window(
                        static_cast<double>(frames),
                        static_cast<int64_t>(sl->slip) + off,
                        doc::media_conform_rate(
                            doc, *a,
                            sl->timeline_lock ? root_fps : eff),
                        &lo, &hi);
                    if (t < lo || t >= hi) {
                        time_culled[fx.id] = 1;
                        continue;
                    }
                    GraphNode srcn;
                    srcn.kind = GraphNode::Kind::Source;
                    srcn.layer_index = static_cast<int>(sli);
                    shifted = add(std::move(srcn), inst,
                                  doc::media_stream_key(
                                      path, sl->id, sl->asset,
                                      sl->timeline_lock, off));
                } else {
                    if (!sl->target) continue;   // dangling: dormant
                    const doc::Look* tl = doc.find_look(sl->target);
                    const doc::Sequence* ts =
                        tl ? nullptr : doc.find_sequence(sl->target);
                    if (!tl && !ts) continue;
                    const uint32_t dur = tl ? tl->duration : ts->duration;
                    const double ratio =
                        doc::entity_fps(doc, sl->target) / eff;
                    const double ct = self.local_time * ratio +
                                      static_cast<double>(off);
                    double lo = 0.0, hi = 0.0;
                    doc::shifted_window(static_cast<double>(dur), off,
                                        ratio, &lo, &hi);
                    if (self.local_time < lo || self.local_time >= hi) {
                        time_culled[fx.id] = 1;
                        continue;
                    }
                    if (self.depth + 1 >= doc::kMaxLookDepth) continue;
                    LookInstance child;
                    child.look = sl->target;
                    child.path = doc::nested_child_path(path, sl->id,
                                                        sl->target, off);
                    child.depth = self.depth + 1;
                    child.local_time = ct;
                    child.local_frame =
                        ct <= 0.0 ? 0u
                                  : static_cast<uint32_t>(std::floor(ct));
                    graph.instances.push_back(child);
                    const int ci =
                        static_cast<int>(graph.instances.size()) - 1;
                    shifted =
                        emit_entity(sl->target, ci, /*is_root=*/false);
                    if (shifted < 0) continue;
                }
                if (doc::layer_has_transform(*sl)) {
                    GraphNode xf;
                    xf.kind = GraphNode::Kind::LayerTransform;
                    xf.layer_index = static_cast<int>(sli);
                    xf.inputs.push_back(shifted);
                    shifted = add(std::move(xf), inst, subject_key(sl->id));
                }
                fx_out[fx.id] = shifted;
            }
        }
    }

    // Stacking order is the link order: the first link composites at bottom.
    std::unordered_map<uint64_t, std::vector<uint64_t>> port_links;
    for (const doc::NodeLink& l : links)
        port_links[(l.to << 8) | l.to_port].push_back(l.from);
    static const std::vector<uint64_t> kNoLinks;
    auto links_into_port =
        [&](uint64_t to, uint32_t port) -> const std::vector<uint64_t>& {
        const auto it = port_links.find((to << 8) | port);
        return it == port_links.end() ? kNoLinks : it->second;
    };
    auto owner_layer_index = [&](uint64_t id) -> size_t {
        if (auto it = owner.find(id); it != owner.end()) return it->second;
        for (size_t k = 0; k < look.layers.size(); ++k)
            if (look.layers[k].id == id) return k;
        return SIZE_MAX;
    };
    // Group input slots resolve like inactive effects: their own fan-in.
    std::unordered_map<uint64_t, char> slot_ids;
    struct FaceWrap {
        size_t layer_index;
        size_t group_index;
        const doc::Group* group;
        bool mixes;
    };
    std::unordered_map<uint64_t, FaceWrap> face_wraps;
    {
        std::unordered_set<uint64_t> driven_groups;
        for (const doc::KeyframeLane& l : look.lanes)
            if ((l.target.effect_id & doc::kGroupParamBit) &&
                !l.keys.empty() && !l.muted)
                driven_groups.insert(l.target.effect_id &
                                     ~doc::kGroupParamBit);
        for (const doc::ModRoute& r : look.mod_routes)
            if ((r.target.effect_id & doc::kGroupParamBit) && r.node)
                driven_groups.insert(r.target.effect_id &
                                     ~doc::kGroupParamBit);
        for (size_t li = 0; li < look.layers.size(); ++li)
            for (size_t gi = 0; gi < look.layers[li].groups.size();
                 ++gi) {
                const doc::Group& g = look.layers[li].groups[gi];
                for (uint64_t s : g.inputs) slot_ids[s] = 1;
                if (g.bypass || strip_effects) continue;
                const bool mixes = g.wet != 1.0f || g.opacity != 1.0f ||
                                   driven_groups.count(g.id) != 0;
                const bool matted = !links_into_port(g.id, 1).empty();
                if (!mixes && !matted) continue;
                const uint64_t face =
                    doc::group_face_member(look.layers[li], g);
                if (face) face_wraps[face] = {li, gi, &g, mixes};
            }
    }
    // True if the feed hangs from a producer that is only off right now.
    std::function<bool(uint64_t, int)> id_culled =
        [&](uint64_t id, int depth) -> bool {
        if (time_culled.count(id)) return true;
        if (depth > 64) return false;
        if (offset_terminal.count(id)) return false;
        if ((owner.count(id) && !fx_out.count(id)) || slot_ids.count(id)) {
            for (uint64_t from : links_into_port(id, 0))
                if (id_culled(from, depth + 1)) return true;
        }
        return false;
    };
    auto port_culled = [&](uint64_t to, uint32_t port) {
        for (uint64_t from : links_into_port(to, port))
            if (id_culled(from, 0)) return true;
        return false;
    };
    // True if a feed still waits on an active effect that has not emitted.
    std::function<bool(uint64_t, int)> id_pending =
        [&](uint64_t id, int depth) -> bool {
        if (depth > 64) return false;
        if (fx_out.count(id) || offset_terminal.count(id)) return false;
        // An ungated head is not the image its consumers must read.
        if (head_ungated.count(id)) return true;
        if (auto it = owner.find(id); it != owner.end()) {
            if (effect_active(id)) return true;
            for (uint64_t from : links_into_port(id, 0))
                if (id_pending(from, depth + 1)) return true;
        } else if (slot_ids.count(id)) {
            for (uint64_t from : links_into_port(id, 0))
                if (id_pending(from, depth + 1)) return true;
        }
        return false;
    };
    auto port_pending = [&](uint64_t to, uint32_t port) {
        for (uint64_t from : links_into_port(to, port))
            if (id_pending(from, 0)) return true;
        return false;
    };
    // Returns the node that makes an id's output; -1 = dormant or culled.
    std::unordered_map<uint64_t, int> merge_memo;
    std::function<int(uint64_t, int)> resolve_node;
    std::function<int(uint64_t, uint32_t, int)> merge_port;
    resolve_node = [&](uint64_t id, int depth) -> int {
        if (auto it = fx_out.find(id); it != fx_out.end())
            return it->second;
        // A shim with no head ends the chain; do not use the unshifted layer.
        if (offset_terminal.count(id)) return -1;
        if (auto it = heads.find(id); it != heads.end()) return it->second;
        if (owner.count(id) || slot_ids.count(id)) {
            // Inactive effects and slots pass their port-0 merge through.
            if (owner.count(id) && effect_active(id))
                return -1;   // starved: never emitted
            if (depth > 64) return -1;
            return merge_port(id, 0, depth + 1);
        }
        return -1;
    };
    merge_port = [&](uint64_t to, uint32_t port, int depth) -> int {
        // The key must stay injective; ports are tiny, so the shift is exact.
        const uint64_t memo_key = (to << 8) | port;
        if (auto it = merge_memo.find(memo_key); it != merge_memo.end())
            return it->second;
        if (depth > 64) return -1;
        struct Feed {
            int node;
            size_t li;
        };
        std::vector<Feed> feeds;
        for (uint64_t from : links_into_port(to, port)) {
            const int n = resolve_node(from, depth + 1);
            if (n < 0) continue;   // dormant / culled: contributes nothing
            feeds.push_back({n, owner_layer_index(from)});
        }
        // The layer matte already gated the head, so the stack carried it
        // down. Alpha-over reveals what is below wherever the gate closed.
        int below = -1;
        for (const Feed& feed : feeds) {
            const int cur = feed.node;
            if (below < 0) {
                below = cur;
            } else {
                GraphNode blend;
                blend.kind = GraphNode::Kind::LayerBlend;
                blend.layer_index =
                    feed.li == SIZE_MAX ? -1
                                        : static_cast<int>(feed.li);
                blend.inputs = {below, cur};
                below = add(std::move(blend), inst,
                            feed.li == SIZE_MAX
                                ? 0
                                : subject_key(look.layers[feed.li].id));
            }
        }
        merge_memo[memo_key] = below;
        return below;
    };

    // matte_node is a port-1 image; the luma extract makes it the gate.
    auto emit_one = [&](size_t li, size_t stack_index, int in_node,
                        int aux_node = -1, int matte_node = -1) {
        const doc::EffectInstance& fx = look.layers[li].stack[stack_index];
        const uint64_t key = subject_key(fx.id);
        int gate = -1;
        if (matte_node >= 0) {
            GraphNode ex;
            ex.kind = GraphNode::Kind::MatteExtract;
            ex.inputs.push_back(matte_node);
            gate = add(std::move(ex), inst);
        }
        // Displace by matte uses the matte as data, not as a gate.
        // A wired aux port wins; extra keeps what the link resolved.
        int extra = aux_node;
        const bool wants_map =
            (fx.type == doc::EffectType::Displace &&
             fx.params.size() > 3 && fx.params[3] >= 0.5f) ||
            (fx.type == doc::EffectType::TimeDisplace &&
             fx.params.size() > 1 && fx.params[1] >= 1.5f);
        if (extra < 0 && wants_map && gate >= 0) extra = gate;
        const int fx_node =
            emit_effect(fx, static_cast<int>(li),
                        static_cast<int>(stack_index), in_node, inst, key,
                        extra);
        int out = fx_node;
        // The matte does not gate if the displace map consumed it.
        if (gate >= 0 && gate != extra) {
            GraphNode apply;
            apply.kind = GraphNode::Kind::MatteApply;
            apply.inputs = {in_node, fx_node, gate};
            out = add(std::move(apply), inst);
        }
        fx_out[fx.id] = out;
        set_tap(fx.id, out);
        return out;
    };

    // Pass B: an effect with no live in-feed is dormant and never emitted.
    // Phase 0 waits for wired aux and matte producers; phase 1 stops waiting.
    std::vector<std::pair<size_t, size_t>> pending;
    for (size_t li = 0; li < look.layers.size(); ++li)
        for (size_t i = 0; i < look.layers[li].stack.size(); ++i) {
            const doc::EffectInstance& fx = look.layers[li].stack[i];
            if (effect_active(fx.id)) pending.emplace_back(li, i);
        }
    // The group dry side is the first slot's feed, or nothing when it is off.
    auto group_dry = [&](const doc::Group& g) {
        if (g.inputs.empty()) return zero();
        const uint64_t slot0 = g.inputs.front();
        int dry = merge_port(slot0, 0, 0);
        if (dry < 0) dry = zero();
        return dry;
    };
    // Consumers read the wrapped output; this overwrites the face fx_out.
    auto wrap_group_face = [&](uint64_t face_id) {
        const auto wit = face_wraps.find(face_id);
        if (wit == face_wraps.end()) return;
        const FaceWrap& fw = wit->second;
        const int raw = fx_out[face_id];
        const int dry = group_dry(*fw.group);
        int out = raw;
        if (fw.mixes) {
            GraphNode mix;
            mix.kind = GraphNode::Kind::GroupMix;
            mix.layer_index = static_cast<int>(fw.layer_index);
            mix.effect_index = static_cast<int>(fw.group_index);
            mix.inputs = {dry, raw};
            out = add(std::move(mix), inst);
        }
        int matte_node = merge_port(fw.group->id, 1, 0);
        if (matte_node < 0 &&
            !links_into_port(fw.group->id, 1).empty() &&
            port_culled(fw.group->id, 1))
            matte_node = zero();
        if (matte_node >= 0) {
            GraphNode ex;
            ex.kind = GraphNode::Kind::MatteExtract;
            ex.inputs.push_back(matte_node);
            const int gate = add(std::move(ex), inst);
            GraphNode apply;
            apply.kind = GraphNode::Kind::MatteApply;
            apply.inputs = {dry, out, gate};
            out = add(std::move(apply), inst);
        }
        fx_out[face_id] = out;
        set_tap(face_id, out);
    };
    // A layer matte gates the HEAD, so the whole stack sees the crop. The
    // composite then alpha-overs, which reveals below where the gate closed.
    auto gate_head = [&](uint64_t layer_id) {
        int m = merge_port(layer_id, 1, 0);
        if (m < 0 && port_culled(layer_id, 1)) m = zero();
        head_ungated.erase(layer_id);
        if (m < 0) return;
        GraphNode ex;
        ex.kind = GraphNode::Kind::MatteExtract;
        ex.inputs.push_back(m);
        const int gate = add(std::move(ex), inst);
        GraphNode apply;
        apply.kind = GraphNode::Kind::MatteApply;
        apply.inputs = {zero(), heads[layer_id], gate};
        const int out = add(std::move(apply), inst);
        heads[layer_id] = out;
        set_tap(layer_id | kThumbSourceBit, out);
    };
    for (int phase = 0; phase < 2; ++phase) {
        bool progress = true;
        while (progress && (!pending.empty() || !head_ungated.empty())) {
            progress = false;
            // Gate first: an effect must never read an ungated head.
            for (const doc::Layer& hl : look.layers) {
                if (!head_ungated.count(hl.id)) continue;
                if (phase == 0 && port_pending(hl.id, 1)) continue;
                gate_head(hl.id);
                progress = true;
            }
            for (auto it = pending.begin(); it != pending.end();) {
                const doc::EffectInstance& fx =
                    look.layers[it->first].stack[it->second];
                if (port_pending(fx.id, 0)) {
                    ++it;
                    continue;
                }
                if (phase == 0 && (port_pending(fx.id, 2) ||
                                   port_pending(fx.id, 1))) {
                    ++it;
                    continue;
                }
                if (phase == 0) {
                    const auto wit = face_wraps.find(fx.id);
                    if (wit != face_wraps.end()) {
                        const doc::Group& wg = *wit->second.group;
                        const bool dry_wait =
                            !wg.inputs.empty() &&
                            port_pending(wg.inputs.front(), 0);
                        if (dry_wait || port_pending(wg.id, 1)) {
                            ++it;
                            continue;
                        }
                    }
                }
                const int in_node = merge_port(fx.id, 0, 0);
                if (in_node < 0) {
                    // No live in-feed: dormant, drop without emitting.
                    it = pending.erase(it);
                    progress = true;
                    continue;
                }
                // An aux producer that is off feeds zeros; unwired stays -1.
                int aux_node = merge_port(fx.id, 2, 0);
                if (aux_node < 0 &&
                    !links_into_port(fx.id, 2).empty() &&
                    port_culled(fx.id, 2))
                    aux_node = zero();
                // A matte producer that is off feeds zeros, so the gate closes.
                int matte_node = merge_port(fx.id, 1, 0);
                if (matte_node < 0 &&
                    !links_into_port(fx.id, 1).empty() &&
                    port_culled(fx.id, 1))
                    matte_node = zero();
                emit_one(it->first, it->second, in_node, aux_node,
                         matte_node);
                wrap_group_face(fx.id);
                it = pending.erase(it);
                progress = true;
            }
        }
    }

    auto resolve = [&](uint64_t id) -> int {
        return resolve_node(id, 0);
    };

    // Pass C: the composite is the fan-in of Output node id 0, port 0.
    int below = merge_port(0, 0, 0);


    // The preview tap resolves in the root instance only; -1 = unresolvable.
    if (is_root && preview_node != 0) {
        int idx = -1;
        if (auto ith = heads.find(preview_node); ith != heads.end()) {
            idx = ith->second;
        } else {
            // A group previews its face member, which is the wrapped output.
            uint64_t id = preview_node;
            if (const uint64_t face =
                    doc::group_face_member(look, preview_node))
                id = face;
            idx = resolve(id);
        }
        if (idx >= 0) graph.preview = idx;
    }
    // The layer tap resolves the last link that leaves the layer chain.
    // It shows the layer pre-blend and pre-matte; a node tap outranks it.
    if (is_root && graph.preview < 0 && preview_layer != 0) {
        size_t want = SIZE_MAX;
        for (size_t k = 0; k < look.layers.size(); ++k)
            if (look.layers[k].id == preview_layer) want = k;
        int idx = -1;
        if (want != SIZE_MAX)
            for (const doc::NodeLink& l : links) {
                if (owner_layer_index(l.from) != want) continue;
                if (l.to != 0 && owner_layer_index(l.to) == want) continue;
                if (int r = resolve(l.from); r >= 0) idx = r;
            }
        if (idx >= 0) graph.preview = idx;
    }
    return below;
}

}  // namespace

bool topo_sort(const std::vector<GraphNode>& nodes, std::vector<int>& order) {
    order.clear();
    const size_t n = nodes.size();
    std::vector<int> indegree(n, 0);
    std::vector<std::vector<int>> consumers(n);
    for (size_t i = 0; i < n; ++i) {
        for (int input : nodes[i].inputs) {
            // A dangling input fails the sort instead of indexing out of range.
            if (input < 0 || static_cast<size_t>(input) >= n) return false;
            indegree[i]++;
            consumers[static_cast<size_t>(input)].push_back(
                static_cast<int>(i));
        }
    }
    std::vector<int> ready;
    for (size_t i = 0; i < n; ++i)
        if (indegree[i] == 0) ready.push_back(static_cast<int>(i));
    while (!ready.empty()) {
        const int node = ready.back();
        ready.pop_back();
        order.push_back(node);
        for (int c : consumers[static_cast<size_t>(node)])
            if (--indegree[static_cast<size_t>(c)] == 0) ready.push_back(c);
    }
    return order.size() == n;
}

RenderGraph compile_graph(const doc::Document& doc, uint64_t root_id,
                          uint32_t frame, uint64_t preview_node,
                          uint64_t preview_layer,
                          uint64_t measure_placement, bool with_before) {
    RenderGraph graph;

    // Instance 0 is the compiled entity; its path is its own id.
    LookInstance root;
    root.look = root_id;
    root.path = root_id;
    root.local_time = static_cast<double>(frame);
    root.local_frame = frame;
    graph.instances.push_back(root);

    Compiler c{doc, graph, preview_node, preview_layer, measure_placement,
               false, doc::entity_fps(doc, root_id), {}, -1};
    const int below = c.emit_entity(root_id, 0, /*is_root=*/true);

    // An unwired Output composites to nothing, never to a raw source.
    graph.output = below >= 0 ? below : c.zero();
    if (graph.preview == graph.output) graph.preview = -1;

    // The before pass re-emits the same entity with effects stripped only.
    // Its Source keys match the main tree, so decoded planes are shared.
    if (with_before) {
        c.strip_effects = true;
        const int before = c.emit_entity(root_id, 0, /*is_root=*/false);
        graph.before = before >= 0 ? before : c.zero();
    }

    graph.valid = topo_sort(graph.nodes, graph.order);
    return graph;
}

}  // namespace looks::gfx
