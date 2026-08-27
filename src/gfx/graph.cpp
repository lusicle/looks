#include "gfx/graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_map>

#include "doc/instances.h"
#include "util/hash.h"

namespace looks::gfx {

namespace {

// Lazily-created shared Flow node (one motion-vector field per frame).
struct FlowSlot {
    int node = -1;
};

// Compiles the nesting tree into one flat node list. Every INSTANCE
// emits its own nodes: the ownership/output maps live inside emit_look,
// so two hops into the same entity can never share entries — which is
// what makes nesting work at all. Sequences emit only Source subtrees
// and plain over-blends; every effect node comes from a look.
struct Compiler {
    const doc::Document& doc;
    RenderGraph& graph;
    uint64_t preview_node = 0;
    uint64_t preview_layer = 0;
    uint64_t measure_placement = 0;
    // The "before" pass: every effect compiles as bypassed while the
    // composition (sources, layer attributes, lane Motion) stays whole.
    bool strip_effects = false;
    // The ROOT entity's effective rate: timeline-locked media conforms
    // against it (locked nodes read the root clock).
    double root_fps = 30.0;
    FlowSlot flow;
    int black_node = -1;

    int add(GraphNode node, int instance, uint64_t key = 0) {
        node.instance = instance;
        node.key = key;
        graph.nodes.push_back(std::move(node));
        return static_cast<int>(graph.nodes.size()) - 1;
    }

    // Empty-viewport node: used ONLY as graph.output when nothing feeds
    // the composite. No effect ever receives it as an input — an unwired
    // node is DORMANT (not emitted, processes nothing), never fed a
    // fabricated frame.
    int black() {
        if (black_node < 0) {
            GraphNode b;
            b.kind = GraphNode::Kind::Generator;   // layer -1 = black
            black_node = add(std::move(b), 0);
        }
        return black_node;
    }

    // Multi-pass effects expand into fixed node sequences. Glow: bright
    // pass -> blur H -> blur V -> composite(dry, blurred). Flow consumers
    // read the shared Flow node as a second input; Displace reads a
    // grayscale displacement map via `extra_input`.
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
            // Quantize and Dither join the flow consumers only in motion-
            // locked dither mode (lock mode).
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

    // Liveness and the shown-block pick are the shared predicates in
    // doc/document.h - identical to the flatten the decode pool and the
    // audio mix run on, and to the monitor's click-pick.
    const doc::Placement* winner_at(
        const std::vector<doc::Placement>& placements,
        const LookInstance& p, double parent_fps) const {
        return doc::placement_winner(doc, placements, p.local_time,
                                     parent_fps);
    }

    // Emits the entity behind a child instance: dispatch by kind.
    int emit_entity(uint64_t id, int inst, bool is_root) {
        if (doc.find_look(id)) return emit_look(id, inst, is_root);
        if (doc.find_sequence(id)) return emit_sequence(id, inst, is_root);
        return -1;
    }

    int emit_look(uint64_t look_id, int inst, bool is_root);
    int emit_sequence(uint64_t seq_id, int inst, bool is_root);
};

// A sequence composites its lanes bottom-up with plain alpha-over: the
// winning block per lane resolves to its entity's composite, and the
// stack is LayerBlend nodes with layer_index -1 (no modes, no masks -
// those live in looks). Audio tracks are not images and emit nothing.
int Compiler::emit_sequence(uint64_t seq_id, int inst, bool is_root) {
    const doc::Sequence& seq = doc.sequence(seq_id);
    const double eff = doc::effective_fps(doc, seq);
    int below = -1;
    for (const doc::SeqTrack& track : seq.tracks) {
        if (track.hidden) continue;
        // Refetched every lane: recursion appends instances, which may
        // reallocate.
        const LookInstance self =
            graph.instances[static_cast<size_t>(inst)];
        if (self.depth + 1 >= doc::kMaxLookDepth) break;
        const doc::Placement* place =
            winner_at(track.placements, self, eff);
        if (!place || !place->target) continue;
        if (!doc.find_look(place->target) &&
            !doc.find_sequence(place->target))
            continue;   // dangling block: dormant
        LookInstance child;
        child.look = place->target;
        child.path =
            doc::seq_child_path(self.path, track.id, place->target);
        child.depth = self.depth + 1;
        // Composed affine map, evaluated on the parent's CONTINUOUS
        // local time and landing in the child's OWN clock (the hop
        // ratio rides the map); only the frame the effects clock on is
        // floored.
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
        // Measure tap: the selected block's content pre-Motion - the
        // monitor's box math applies the placement transform itself.
        if (is_root && measure_placement && place->id == measure_placement)
            graph.measure = out;
        // Placement composition: stateless canvas geometry and opacity
        // are attributes OF the lane's over-composite - the blend
        // samples the lane image through the placement affine while
        // compositing. No transform node exists at sequence level
        // (sequences never own effects). A bottom lane with Motion or
        // reduced opacity blends over transparent black so both are
        // honored there too.
        const float popa = std::clamp(place->opacity, 0.0f, 1.0f);
        const bool moved = doc::placement_has_transform(*place);
        if (below < 0 && popa >= 1.0f && !moved) {
            below = out;
        } else {
            GraphNode blend;
            blend.kind = GraphNode::Kind::LayerBlend;
            blend.layer_index = -1;   // plain alpha-over
            blend.p_opacity = popa;
            if (moved) {
                blend.p_shift_x = place->pos_x;
                blend.p_shift_y = place->pos_y;
                blend.p_scale = place->scale;
                blend.p_rotate = place->rotate * doc::kDeg2Rad;
                blend.p_anchor_x = place->anchor_x;
                blend.p_anchor_y = place->anchor_y;
            }
            blend.inputs = {below < 0 ? black() : below, out};
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

    // TRUE GRAPH: effect wiring and the final composite come from the
    // link table; an empty table compiles through the synthesized
    // stack-order chain.
    std::vector<doc::NodeLink> links_synth;
    const std::vector<doc::NodeLink>& links =
        doc::effective_links(look, links_synth);
    auto link_into = [&](uint64_t to, uint32_t port) -> uint64_t {
        for (const doc::NodeLink& l : links)
            if (l.to == to && l.to_port == port) return l.from;
        return 0;
    };

    // Ownership + activity. Solo mutes within the owner layer.
    std::unordered_map<uint64_t, size_t> owner;    // fx id → layer index
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
        // Audio modifiers are image-identity: the image graph routes
        // around them like bypassed nodes; the audio flatten collects
        // them into the voice's DSP op list instead. Offset shims never
        // dispatch either - pass A2 turns the source-adjacent ones into
        // shifted source reads, the rest route through.
        if (doc::is_audio_effect(fx.type) ||
            fx.type == doc::EffectType::Offset)
            return false;
        return look.layers[li].visible && !fx.bypass &&
               !(layer_solo[li] && !fx.solo) &&
               !group_bypassed_in(li, fx.group_id);
    };

    // Pass A: layer source heads, all in LOCKSTEP with this look's clock.
    // A source past its playable end emits nothing — but it is
    // remembered: a WIRE to a time-culled producer is a closed gate
    // (transparent black), never "unwired". That distinction is what makes
    // masking across time work: when the mask's media ends, the masked
    // contribution disappears instead of popping to full.
    std::unordered_map<uint64_t, int> heads;   // layer id → node index
    std::unordered_map<uint64_t, char> time_culled;
    const LookInstance self = graph.instances[static_cast<size_t>(inst)];
    for (size_t li = 0; li < look.layers.size(); ++li) {
        const doc::Layer& layer = look.layers[li];
        if (!layer.visible) continue;
        int cur = -1;
        if (doc::layer_is_media(layer)) {
            // An unbound media node is DORMANT, the way an unwired port
            // is. A bound one plays its media from local 0 (plus slip);
            // past the media it is a CLOSED GATE. A timeline-locked
            // node reads (and windows) on the ROOT clock instead.
            if (!layer.asset) continue;
            const doc::Asset* a = doc.find_asset(layer.asset);
            // A DANGLING id (asset removed) is dormant exactly like an
            // unbound node. An asset with no picture at all (audio
            // import without cover art: no frames, no dimensions) has
            // NO image head: the node is image-dormant and only the
            // audio walk carries it. The flatten mirrors both exactly.
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
            // REFERENCE SOURCE: the first media source emitted anywhere — the
            // bottom-most, earliest chain — anchors the A/B wipe and the
            // shared motion field.
            if (graph.source < 0) graph.source = cur;
        } else if (doc::layer_is_nested(layer)) {
            // A nested entity plays 1:1 with this clock. An explicit
            // duration cuts it (closed gate past the end); a dangling
            // target is dormant; depth past the bound is dormant.
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
            // Lockstep is 1:1 in TIME: the hop ratio scales the child's
            // own clock.
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
    }

    std::unordered_map<uint64_t, int> fx_out;   // fx id → output node

    // Pass A2: OFFSET shims. A live Offset wired DIRECTLY onto a source
    // layer becomes a SHIFTED read of that source - media re-keys its
    // decode stream, a nested ref re-instances on the shifted clock -
    // and settles into fx_out like a producer. Wired anywhere else (an
    // effect, a generator, dangling) it is a pass-through wire. The
    // flatten mirrors this exactly (walk_look's vshifts + voice offset).
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
                const uint64_t src = link_into(fx.id, 0);
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
                    // Dangling or audio-only: (image-)dormant, like the
                    // base.
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

    // ---- link-order fan-in resolution. STACKING ORDER IS THE LINK
    // ORDER: a port's feeds composite bottom -> top in link-vector
    // order (first link = bottom; new wires append, so newest lands on
    // top), each feed through its OWNER layer's blend/opacity and
    // gated by that layer's port-1 matte. The Output's In is simply
    // the composite's fan-in - effect ports merge by the same one
    // rule - and an inactive effect passes its own In merge through.
    // Indexed once: the recursive resolvers and the fixpoint loop below
    // hit this superlinearly. One forward pass keeps link-vector order
    // inside each key (stacking order). Key packing matches merge_memo:
    // ports are tiny, so shifting is exact.
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
    // Does this feed hang (possibly through effects, settled or starved
    // alike) from a producer that is merely OFF right now? Culled reads
    // as a CLOSED GATE (black), never as unwired.
    std::function<bool(uint64_t, int)> id_culled =
        [&](uint64_t id, int depth) -> bool {
        if (time_culled.count(id)) return true;
        if (depth > 64) return false;
        if (offset_terminal.count(id)) return false;
        if (owner.count(id) && !fx_out.count(id)) {
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
    // Is a feed still waiting on an ACTIVE effect that has not emitted?
    // (Dormant chains never settle and read as absent; cycles bottom
    // out on the depth guard and resolve to nothing.)
    std::function<bool(uint64_t, int)> id_pending =
        [&](uint64_t id, int depth) -> bool {
        if (depth > 64) return false;
        if (fx_out.count(id) || offset_terminal.count(id)) return false;
        if (auto it = owner.find(id); it != owner.end()) {
            if (effect_active(id)) return true;
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
    // The node producing an id's output: emitted effects and shims from
    // fx_out, layer heads from Pass A, inactive effects as the merge of
    // their own In fan-in. -1 = dormant / culled / unsettled: the
    // contribution simply does not exist.
    std::unordered_map<uint64_t, int> merge_memo;
    std::function<int(uint64_t, int)> resolve_node;
    std::function<int(uint64_t, uint32_t, int)> merge_port;
    resolve_node = [&](uint64_t id, int depth) -> int {
        if (auto it = fx_out.find(id); it != fx_out.end())
            return it->second;
        // A shim with no head is time-culled or dormant: the chain
        // ends here - never fall through to the unshifted layer.
        if (offset_terminal.count(id)) return -1;
        if (auto it = heads.find(id); it != heads.end()) return it->second;
        if (owner.count(id)) {
            if (effect_active(id)) return -1;   // starved: never emitted
            if (depth > 64) return -1;
            return merge_port(id, 0, depth + 1);
        }
        return -1;
    };
    merge_port = [&](uint64_t to, uint32_t port, int depth) -> int {
        // Injective key: hash_combine folds small consecutive ids onto
        // each other ((4,1) collided with (7,0)); ports are tiny, so
        // shifting is exact.
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
        // The owner-layer matte gates a contribution ONCE, at the
        // COMPOSITE - a mid-graph merge is plain wiring (mask an effect
        // feed by wiring the mask into ITS port 1), or the same matte
        // would re-apply at every pass-through hop of the chain.
        const bool composite = to == 0 && port == 0;
        int below = -1;
        for (const Feed& feed : feeds) {
            int cur = feed.node;
            // The feed's owner-layer matte (its port-1 fan-in) gates
            // the whole contribution; a mask whose media is off right
            // now gates it OUT (cross-time masking).
            int gate_out = -1;
            if (composite && feed.li != SIZE_MAX) {
                const doc::Layer& fl = look.layers[feed.li];
                int idx = merge_port(fl.id, 1, depth + 1);
                if (idx < 0 && !links_into_port(fl.id, 1).empty() &&
                    port_culled(fl.id, 1))
                    idx = black();
                if (idx >= 0) {
                    GraphNode ex;
                    ex.kind = GraphNode::Kind::MatteExtract;
                    ex.inputs.push_back(idx);
                    gate_out = add(std::move(ex), inst);
                }
            }
            if (below < 0) {
                if (gate_out >= 0) {
                    // Nothing below: the matte reveals TRANSPARENT
                    // black, the premultiplied zero.
                    GraphNode apply;
                    apply.kind = GraphNode::Kind::MatteApply;
                    apply.inputs = {black(), cur, gate_out};
                    cur = add(std::move(apply), inst);
                }
                below = cur;
            } else {
                GraphNode blend;
                blend.kind = GraphNode::Kind::LayerBlend;
                blend.layer_index =
                    feed.li == SIZE_MAX ? -1
                                        : static_cast<int>(feed.li);
                blend.inputs = {below, cur};
                int blended =
                    add(std::move(blend), inst,
                        feed.li == SIZE_MAX
                            ? 0
                            : subject_key(look.layers[feed.li].id));
                if (gate_out >= 0) {
                    GraphNode apply;
                    apply.kind = GraphNode::Kind::MatteApply;
                    apply.inputs = {below, blended, gate_out};
                    blended = add(std::move(apply), inst);
                }
                below = blended;
            }
        }
        merge_memo[memo_key] = below;
        return below;
    };

    // One effect emission, with the In node resolved from the links.
    // matte_node (a port-1 IMAGE link — masks ARE images) runs through
    // the luma extract so the wired image gates the effect.
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
        // Displace-by-matte: the matte grayscale is a second input, not a
        // gate; Time Displace mode 2 consumes it the same way (per-pixel
        // delay map). A wired aux port wins — extra stays what the link
        // resolved.
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
        // The matte gates the effect UNLESS it was consumed as the
        // displace map (never double-apply). A wired aux (b/map) does
        // NOT suppress gating.
        if (gate >= 0 && gate != extra) {
            GraphNode apply;
            apply.kind = GraphNode::Kind::MatteApply;
            apply.inputs = {in_node, fx_node, gate};
            out = add(std::move(apply), inst);
        }
        fx_out[fx.id] = out;
        return out;
    };

    // Pass B: effects in link-topological order. An effect with NO
    // live in-feed (or fan-in that dangles into nothing) is DORMANT:
    // never emitted, processes nothing, and anything fed only by it
    // starves at the fixpoint and stays dormant too — an unconnected
    // node must not run. Cycle-stuck nodes starve the same way. Aux and
    // matte are optional: phase 0 waits for their wired producers so
    // the node indices exist; phase 1 re-runs treating fan-ins fed by
    // dormant chains as unwired instead of starving the consumer.
    std::vector<std::pair<size_t, size_t>> pending;
    for (size_t li = 0; li < look.layers.size(); ++li)
        for (size_t i = 0; i < look.layers[li].stack.size(); ++i) {
            const doc::EffectInstance& fx = look.layers[li].stack[i];
            if (effect_active(fx.id)) pending.emplace_back(li, i);
        }
    for (int phase = 0; phase < 2; ++phase) {
        bool progress = true;
        while (progress && !pending.empty()) {
            progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                const doc::EffectInstance& fx =
                    look.layers[it->first].stack[it->second];
                if (port_pending(fx.id, 0)) {
                    ++it;   // an in-feed not settled yet — or starved
                    continue;
                }
                if (phase == 0 && (port_pending(fx.id, 2) ||
                                   port_pending(fx.id, 1))) {
                    ++it;
                    continue;
                }
                const int in_node = merge_port(fx.id, 0, 0);
                if (in_node < 0) {
                    // No live in-feed: dormant, drop without emitting.
                    it = pending.erase(it);
                    progress = true;
                    continue;
                }
                // Aux (b/map) fan-in: a producer that is merely off
                // right now feeds black (a zero map); fed by nothing
                // reads as unwired.
                int aux_node = merge_port(fx.id, 2, 0);
                if (aux_node < 0 &&
                    !links_into_port(fx.id, 2).empty() &&
                    port_culled(fx.id, 2))
                    aux_node = black();
                // Port-1 matte (masks ARE images): the mask's media
                // ended = luma 0, gate CLOSED — the effect reads dry
                // instead of running ungated.
                int matte_node = merge_port(fx.id, 1, 0);
                if (matte_node < 0 &&
                    !links_into_port(fx.id, 1).empty() &&
                    port_culled(fx.id, 1))
                    matte_node = black();
                emit_one(it->first, it->second, in_node, aux_node,
                         matte_node);
                it = pending.erase(it);
                progress = true;
            }
        }
    }

    // Resolves any document node id to its graph output. Dormant /
    // unresolvable = -1: the contribution simply does not exist
    // (nothing is fabricated in its place).
    auto resolve = [&](uint64_t id) -> int {
        return resolve_node(id, 0);
    };

    // Pass C: the composite IS the Output port's fan-in - link order,
    // bottom -> top, each contribution through its owner layer's
    // blend/opacity/matte (merge_port's one rule).
    int below = merge_port(0, 0, 0);

    // Viewport tap: the OUTPUT stays the real composite; the big preview
    // publishes graph.preview instead when set. Resolved in the root
    // instance only — you preview the entity you are editing. Dormant /
    // boundary / unresolvable selections leave it at -1.
    if (is_root && preview_node != 0) {
        int idx = -1;
        if (auto ith = heads.find(preview_node); ith != heads.end()) {
            idx = ith->second;   // a source node: its transformed head
        } else {
            // Effect — or a group, which previews its bound out member.
            uint64_t id = preview_node;
            for (const doc::Layer& l : look.layers)
                for (const doc::Group& g : l.groups)
                    if (g.id == preview_node) {
                        uint64_t last_m = 0, bind = 0;
                        for (const doc::EffectInstance& e : l.stack)
                            if (e.group_id == g.id) {
                                last_m = e.id;
                                if (e.id == g.face_out) bind = e.id;
                            }
                        id = bind ? bind : last_m;
                    }
            idx = resolve(id);
        }
        if (idx >= 0) graph.preview = idx;
    }
    // LAYER tap (editor selection): the layer's whole contribution —
    // resolve the last link leaving its chain for the Output or another
    // layer, pre-blend and pre-matte. The matte gates the layer INTO the
    // composite and comes from another chain, so the layer alone shows
    // without it. A node selection outranks this; dormant or unwired
    // layers resolve to nothing and keep the composite.
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
            // A dangling input index is a compiler bug; failing the
            // sort (black frame) beats indexing out of range.
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

    // The compiled entity is instance 0: its path is its own id, its
    // local time is the timeline frame. Nested hops hang off it.
    LookInstance root;
    root.look = root_id;
    root.path = root_id;
    root.local_time = static_cast<double>(frame);
    root.local_frame = frame;
    graph.instances.push_back(root);

    Compiler c{doc, graph, preview_node, preview_layer, measure_placement,
               false, doc::entity_fps(doc, root_id), {}, -1};
    const int below = c.emit_entity(root_id, 0, /*is_root=*/true);

    // Unwired Output (flat graph): nothing feeds the composite, so
    // the composite is BLACK — `output = 0` passed the raw source
    // through, which is chain residue, not a node graph.
    graph.output = below >= 0 ? below : c.black();
    if (graph.preview == graph.output) graph.preview = -1;

    // The A/B "before": the SAME entity re-emitted with effects
    // stripped. Composition attributes (arrangement, Motion, opacity,
    // layer transforms) are not effects and must survive the wipe;
    // Source keys match the main tree, so decoded planes are shared.
    if (with_before) {
        c.strip_effects = true;
        const int before = c.emit_entity(root_id, 0, /*is_root=*/false);
        graph.before = before >= 0 ? before : c.black();
    }

    graph.valid = topo_sort(graph.nodes, graph.order);
    return graph;
}

}  // namespace looks::gfx
