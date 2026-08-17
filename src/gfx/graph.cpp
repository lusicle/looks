#include "gfx/graph.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

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

    // Is a placement playing at its instance's local time? Both bounds are
    // whole frames, so testing the continuous time and testing its floor
    // agree exactly - which is what keeps this identical to the flatten
    // the decode pool and the audio mix run on (doc/instances.h).
    bool active_at(const doc::Placement& place, const LookInstance& p) const {
        const uint32_t len = doc::source_length(doc, place);
        if (p.local_time < static_cast<double>(place.t_in)) return false;
        const uint32_t end = doc::placement_end(place, len);
        return end == 0 || p.local_time < static_cast<double>(end);
    }

    // The block a lane SHOWS at this frame: among the active ones, the
    // latest-starting wins (an overlap reads as the incoming block taking
    // over at its in-point), list order breaking ties. Audio does not
    // pick - every active placement sums in the mix.
    const doc::Placement* winner_at(
        const std::vector<doc::Placement>& placements,
        const LookInstance& p) const {
        const doc::Placement* best = nullptr;
        for (const doc::Placement& place : placements) {
            if (!active_at(place, p)) continue;
            if (!best || place.t_in >= best->t_in) best = &place;
        }
        return best;
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
    int below = -1;
    for (const doc::SeqTrack& track : seq.tracks) {
        // Refetched every lane: recursion appends instances, which may
        // reallocate.
        const LookInstance self =
            graph.instances[static_cast<size_t>(inst)];
        if (self.depth + 1 >= doc::kMaxLookDepth) break;
        const doc::Placement* place = winner_at(track.placements, self);
        if (!place || !place->target) continue;
        if (!doc.find_look(place->target) &&
            !doc.find_sequence(place->target))
            continue;   // dangling block: dormant
        LookInstance child;
        child.look = place->target;
        // The lane AND the target fold into the path: a razor moves
        // nothing, two different targets cut on one lane stay distinct.
        child.path = hash_combine(hash_combine(self.path, track.id),
                                  place->target);
        child.depth = self.depth + 1;
        // Composed affine map, evaluated on the parent's CONTINUOUS local
        // time; only the frame the effects clock on is floored.
        child.local_time =
            doc::placement_source_frame(*place, self.local_time);
        child.local_frame = child.local_time <= 0.0
            ? 0u
            : static_cast<uint32_t>(std::floor(child.local_time));
        graph.instances.push_back(child);
        const int ci = static_cast<int>(graph.instances.size()) - 1;
        const int out = emit_entity(place->target, ci, /*is_root=*/false);
        if (out < 0) continue;
        // LANE tap (timeline selection): the lane's own output before it
        // stacks into the film.
        if (is_root && preview_layer == track.id && graph.preview < 0)
            graph.preview = out;
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
                blend.p_rotate = place->rotate * 0.01745329252f;
            }
            blend.inputs = {below < 0 ? black() : below, out};
            below = add(std::move(blend), inst);
        }
    }
    return below;
}

int Compiler::emit_look(uint64_t look_id, int inst, bool is_root) {
    const doc::Look& look = doc.look(look_id);
    // By value: recursion appends instances, which may reallocate.
    const uint64_t path = graph.instances[static_cast<size_t>(inst)].path;
    auto subject_key = [&](uint64_t id) { return hash_combine(path, id); };

    // TRUE GRAPH: effect wiring and the final composite come from the
    // link table; an empty table compiles through the synthesized
    // stack-order chain.
    const std::vector<doc::NodeLink> links =
        look.links.empty() ? doc::synthesize_links(look) : look.links;
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
        if (doc::layer_is_clip(layer)) {
            // An unbound clip node is DORMANT, the way an unwired port
            // is. A bound one plays its media from local 0 (plus slip);
            // past the media it is a CLOSED GATE.
            if (!layer.asset) continue;
            const doc::Asset* a = doc.find_asset(layer.asset);
            const uint32_t frames = a ? a->frame_count : 0;
            if (frames) {
                const uint32_t playable =
                    frames > layer.slip ? frames - layer.slip : 0;
                if (self.local_time >= static_cast<double>(playable)) {
                    time_culled[layer.id] = 1;
                    continue;
                }
            }
            GraphNode src;
            src.kind = GraphNode::Kind::Source;
            src.layer_index = static_cast<int>(li);
            cur = add(std::move(src), inst,
                      hash_combine(subject_key(layer.id), layer.asset));
            // REFERENCE SOURCE: the first clip emitted anywhere — the
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
            if (dur && self.local_time >= static_cast<double>(dur)) {
                time_culled[layer.id] = 1;
                continue;
            }
            if (self.depth + 1 >= doc::kMaxLookDepth) continue;
            LookInstance child;
            child.look = layer.target;
            child.path = hash_combine(hash_combine(path, layer.id),
                                      layer.target);
            child.depth = self.depth + 1;
            child.local_time = self.local_time;
            child.local_frame = self.local_frame;
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

    // Effective node behind an id: follow In links THROUGH inactive
    // effects (a bypassed node passes its input along) to a head, an
    // active effect, or the shared source.
    auto effective_from = [&](uint64_t cur) -> uint64_t {
        for (int guard = 0; guard < 512; ++guard) {
            if (owner.find(cur) == owner.end()) return cur;   // head / 0
            if (effect_active(cur)) return cur;
            cur = link_into(cur, 0);
        }
        return 0;
    };
    auto upstream_of = [&](uint64_t id) {
        return effective_from(link_into(id, 0));
    };
    // Does this feed hang from a producer that is merely OFF right now?
    // Walks the In-chain through effects (settled or starved alike) to the
    // head it hangs from.
    auto feed_time_culled = [&](uint64_t id) {
        for (int guard = 0; guard < 512 && id; ++guard) {
            if (time_culled.count(id)) return true;
            if (owner.find(id) == owner.end()) return false;   // live head
            id = link_into(id, 0);
        }
        return false;
    };

    // One effect emission, with the In node resolved from the links.
    // matte_node (a port-1 IMAGE link — masks ARE images) runs through
    // the luma extract so the wired image gates the effect.
    std::unordered_map<uint64_t, int> fx_out;   // fx id → output node
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
    // in-wire (or a chain that dangles into nothing) is DORMANT: never
    // emitted, processes nothing, and anything fed only by it starves at
    // the fixpoint and stays dormant too — an unconnected node must not
    // run. Cycle-stuck nodes starve the same way. Aux is optional:
    // phase 0 waits for wired aux producers so their node index exists;
    // phase 1 re-runs treating aux fed by dormant chains as unwired
    // instead of starving the consumer.
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
                const uint64_t up_link = link_into(fx.id, 0);
                const uint64_t up = up_link ? effective_from(up_link) : 0;
                const bool up_is_fx = owner.find(up) != owner.end();
                if (up_is_fx && fx_out.find(up) == fx_out.end()) {
                    ++it;   // not settled yet — or dormant: starves out
                    continue;
                }
                int in_node = -1;
                if (up_is_fx)
                    in_node = fx_out[up];
                else if (auto ith = heads.find(up); ith != heads.end())
                    in_node = ith->second;
                if (in_node < 0) {
                    // No in-wire: dormant, drop without emitting.
                    it = pending.erase(it);
                    progress = true;
                    continue;
                }
                const uint64_t aux_link = link_into(fx.id, 2);
                const uint64_t aux =
                    aux_link ? effective_from(aux_link) : 0;
                const bool aux_is_fx = owner.find(aux) != owner.end();
                if (phase == 0 && aux_link && aux_is_fx &&
                    fx_out.find(aux) == fx_out.end()) {
                    ++it;
                    continue;
                }
                int aux_node = -1;
                if (aux_link) {
                    if (aux_is_fx) {
                        if (auto ita = fx_out.find(aux);
                            ita != fx_out.end())
                            aux_node = ita->second;
                    } else if (auto ith = heads.find(aux);
                               ith != heads.end()) {
                        aux_node = ith->second;
                    }
                    // A producer that is merely off right now feeds black
                    // (a zero map); fed by nothing reads as unwired.
                    if (aux_node < 0 && feed_time_culled(aux_link))
                        aux_node = black();
                }
                // Port-1 matte (masks ARE images): settles exactly like
                // aux — phase 0 waits for the wired producer, a dormant
                // feed reads as unwired (no gate, never fabricated).
                const uint64_t matte_link = link_into(fx.id, 1);
                const uint64_t matte =
                    matte_link ? effective_from(matte_link) : 0;
                const bool matte_is_fx =
                    owner.find(matte) != owner.end();
                if (phase == 0 && matte_link && matte_is_fx &&
                    fx_out.find(matte) == fx_out.end()) {
                    ++it;
                    continue;
                }
                int matte_node = -1;
                if (matte_link) {
                    if (matte_is_fx) {
                        if (auto itm = fx_out.find(matte);
                            itm != fx_out.end())
                            matte_node = itm->second;
                    } else if (auto ith = heads.find(matte);
                               ith != heads.end()) {
                        matte_node = ith->second;
                    }
                    // The mask's media ended: luma 0, gate CLOSED — the
                    // effect reads dry instead of running ungated.
                    if (matte_node < 0 && feed_time_culled(matte_link))
                        matte_node = black();
                }
                emit_one(it->first, it->second, in_node, aux_node,
                         matte_node);
                it = pending.erase(it);
                progress = true;
            }
        }
    }

    // Resolves any document node id to its graph output for compositing.
    // Dormant / unresolvable = -1: the contribution simply does not
    // exist (nothing is fabricated in its place).
    auto resolve = [&](uint64_t id) -> int {
        if (effect_active(id))
            if (auto it = fx_out.find(id); it != fx_out.end())
                return it->second;
        const uint64_t up = owner.count(id) ? upstream_of(id) : id;
        if (auto it = fx_out.find(up); it != fx_out.end()) return it->second;
        if (auto it = heads.find(up); it != heads.end()) return it->second;
        return -1;
    };

    // Pass C: the composite — Output links in table order, bottom-up.
    // Each contribution blends with its OWNER layer's mode/opacity and
    // layer matte.
    int below = -1;
    for (const doc::NodeLink& l : links) {
        if (l.to != 0 || l.to_port != 0) continue;
        size_t li = SIZE_MAX;
        if (auto ito = owner.find(l.from); ito != owner.end()) {
            li = ito->second;
        } else {
            for (size_t k = 0; k < look.layers.size(); ++k)
                if (look.layers[k].id == l.from) li = k;
        }
        if (li == SIZE_MAX || !look.layers[li].visible) continue;
        const doc::Layer& layer = look.layers[li];

        // Every contribution resolves through the links — the adjustment
        // special case is gone (source in, output out, everything
        // between wires freely; merges are Blend nodes).
        // A dormant chain contributes NOTHING.
        int cur = resolve(l.from);
        if (cur < 0) continue;

        // Layer matte: a port-1 IMAGE link on the layer gates the whole
        // contribution — the bottom contribution reveals the raw source
        // where the matte is black. A mask whose media is off right now
        // gates the contribution OUT (cross-time masking).
        int gate_out = -1;
        if (const uint64_t lm_link = link_into(layer.id, 1)) {
            const uint64_t lm = effective_from(lm_link);
            int idx = -1;
            if (auto itf = fx_out.find(lm); itf != fx_out.end())
                idx = itf->second;
            else if (auto ith = heads.find(lm); ith != heads.end())
                idx = ith->second;
            if (idx < 0 && feed_time_culled(lm_link)) idx = black();
            if (idx >= 0) {
                GraphNode ex;
                ex.kind = GraphNode::Kind::MatteExtract;
                ex.inputs.push_back(idx);
                gate_out = add(std::move(ex), inst);
            }
        }
        if (below < 0) {
            if (gate_out >= 0) {
                // Nothing below: the matte reveals TRANSPARENT black, the
                // premultiplied zero. (Pre-alpha this revealed the raw
                // clip, which was chain residue.)
                GraphNode apply;
                apply.kind = GraphNode::Kind::MatteApply;
                apply.inputs = {black(), cur, gate_out};
                cur = add(std::move(apply), inst);
            }
            below = cur;
        } else {
            GraphNode blend;
            blend.kind = GraphNode::Kind::LayerBlend;
            blend.layer_index = static_cast<int>(li);
            blend.inputs = {below, cur};
            int blended = add(std::move(blend), inst, subject_key(layer.id));
            if (gate_out >= 0) {
                GraphNode apply;
                apply.kind = GraphNode::Kind::MatteApply;
                apply.inputs = {below, blended, gate_out};
                blended = add(std::move(apply), inst);
            }
            below = blended;
        }
    }

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
        auto layer_of = [&](uint64_t id) -> size_t {
            if (auto it = owner.find(id); it != owner.end())
                return it->second;
            for (size_t k = 0; k < look.layers.size(); ++k)
                if (look.layers[k].id == id) return k;
            return SIZE_MAX;
        };
        int idx = -1;
        if (want != SIZE_MAX)
            for (const doc::NodeLink& l : links) {
                if (layer_of(l.from) != want) continue;
                if (l.to != 0 && layer_of(l.to) == want) continue;
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
               {}, -1};
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
