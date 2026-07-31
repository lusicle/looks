#include "gfx/graph.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace looks::gfx {

namespace {

// Lazily-created shared Flow node (one motion-vector field per frame).
struct FlowSlot {
    int node = -1;
};

// Multi-pass effects expand into fixed node sequences. Glow: bright pass ->
// blur H -> blur V -> composite(dry, blurred). Flow consumers read the
// shared Flow node as a second input; Displace reads its mask's grayscale
// as a displacement map via `extra_input` (spec §6.2 second-input
// displacement — stack effects only, mask chains pass -1).
int emit_effect(std::vector<GraphNode>& nodes, const doc::EffectInstance& fx,
                int layer_index, int stack_index, int mask_index,
                int chain_index, int upstream, FlowSlot& flow,
                int extra_input = -1) {
    auto add = [&](GraphNode node) {
        nodes.push_back(std::move(node));
        return static_cast<int>(nodes.size()) - 1;
    };
    auto make = [&](int pass, std::vector<int> inputs) {
        GraphNode n;
        n.kind = GraphNode::Kind::Effect;
        n.layer_index = layer_index;
        n.effect_index = stack_index;
        n.mask_index = mask_index;
        n.chain_index = chain_index;
        n.pass_index = pass;
        n.inputs = std::move(inputs);
        return n;
    };

    if (fx.type == doc::EffectType::Glow) {
        const int bright = add(make(0, {upstream}));
        const int blur_h = add(make(1, {bright}));
        const int blur_v = add(make(2, {blur_h}));
        return add(make(3, {upstream, blur_v}));
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
        // locked dither mode (spec §6.2 lock mode).
        if (flow.node < 0) {
            GraphNode f;
            f.kind = GraphNode::Kind::Flow;
            flow.node = add(std::move(f));
        }
        return add(make(0, {upstream, flow.node}));
    }
    if (doc::effect_aux_port(fx.type) && extra_input >= 0)
        return add(make(0, {upstream, extra_input}));
    return add(make(0, {upstream}));
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
            consumers[static_cast<size_t>(input)].push_back(static_cast<int>(i));
        }
    }

    std::vector<int> ready;
    for (size_t i = 0; i < n; ++i)
        if (indegree[i] == 0) ready.push_back(static_cast<int>(i));

    // Pop the lowest index first: deterministic order regardless of how the
    // compiler happened to emit nodes.
    while (!ready.empty()) {
        int best = 0;
        for (size_t i = 1; i < ready.size(); ++i)
            if (ready[i] < ready[best]) best = static_cast<int>(i);
        const int node = ready[best];
        ready.erase(ready.begin() + best);
        order.push_back(node);
        for (int c : consumers[static_cast<size_t>(node)])
            if (--indegree[static_cast<size_t>(c)] == 0) ready.push_back(c);
    }
    return order.size() == n;
}

RenderGraph compile_graph(const doc::Document& doc,
                          uint64_t overlay_mask_id,
                          uint64_t preview_node) {
    RenderGraph graph;

    GraphNode source;
    source.kind = GraphNode::Kind::Source;
    graph.nodes.push_back(std::move(source));

    auto add_node = [&](GraphNode node) {
        graph.nodes.push_back(std::move(node));
        return static_cast<int>(graph.nodes.size()) - 1;
    };
    FlowSlot flow;

    // LEGACY mask entities (spec §8) compile once, shared by reference;
    // `building` breaks combine cycles. The MODERN matte path is a
    // port-1 image link (docs/flow_canvas.md v5.2: masks ARE images) —
    // handled in pass B; mask_id remains for old documents.
    std::unordered_map<uint64_t, int> mask_outputs;
    std::unordered_set<uint64_t> building;
    std::function<int(int)> build_mask = [&](int mask_index) -> int {
        const doc::Mask& mask = doc.masks[static_cast<size_t>(mask_index)];
        if (auto it = mask_outputs.find(mask.id); it != mask_outputs.end())
            return it->second;
        building.insert(mask.id);

        auto post_node = [&](GraphNode::Kind kind, int input) {
            GraphNode n;
            n.kind = kind;
            n.mask_index = mask_index;
            n.inputs.push_back(input);
            return add_node(std::move(n));
        };

        int out;
        if (mask.type == doc::MaskType::Shape) {
            GraphNode shape;
            shape.kind = GraphNode::Kind::MaskShape;
            shape.mask_index = mask_index;
            out = add_node(std::move(shape));
        } else {
            // Source (spec §8: another layer's source, a generator, an
            // external mask-source video, or the shared clip source) ->
            // mini chain -> extract -> expand/contract -> feather.
            // Motion masks read the shared flow field instead.
            int cur = 0;
            if (mask.type == doc::MaskType::Motion) {
                if (flow.node < 0) {
                    GraphNode f;
                    f.kind = GraphNode::Kind::Flow;
                    flow.node = add_node(std::move(f));
                }
                cur = flow.node;
            } else if (mask.source_gen != 0) {
                GraphNode gen;
                gen.kind = GraphNode::Kind::Generator;
                gen.mask_index = mask_index;
                cur = add_node(std::move(gen));
            } else if (mask.source_layer_id != 0) {
                for (size_t li = 0; li < doc.layers.size(); ++li) {
                    const doc::Layer& src_layer = doc.layers[li];
                    if (src_layer.id != mask.source_layer_id) continue;
                    if (src_layer.source == doc::LayerSourceKind::Clip) {
                        if (doc::layer_has_trim(src_layer)) {
                            GraphNode s;
                            s.kind = GraphNode::Kind::Source;
                            s.layer_index = static_cast<int>(li);
                            cur = add_node(std::move(s));
                        }
                        // else: the shared source, node 0.
                    } else if (src_layer.source !=
                               doc::LayerSourceKind::Adjustment) {
                        GraphNode gen;
                        gen.kind = GraphNode::Kind::Generator;
                        gen.layer_index = static_cast<int>(li);
                        cur = add_node(std::move(gen));
                    }
                    break;
                }
            } else if (!mask.source_path.empty()) {
                GraphNode src;
                src.kind = GraphNode::Kind::MaskSource;
                src.mask_index = mask_index;
                cur = add_node(std::move(src));
            }
            if (mask.type != doc::MaskType::Motion) {
                for (size_t c = 0; c < mask.chain.size(); ++c) {
                    if (mask.chain[c].bypass) continue;
                    cur = emit_effect(graph.nodes, mask.chain[c], -1, -1,
                                      mask_index, static_cast<int>(c), cur,
                                      flow);
                }
            }
            cur = post_node(GraphNode::Kind::MaskExtract, cur);
            if (mask.grow_px >= 0.5f || mask.grow_px <= -0.5f) {
                cur = post_node(GraphNode::Kind::MaskMorphH, cur);
                cur = post_node(GraphNode::Kind::MaskMorphV, cur);
            }
            if (mask.blur_px >= 0.5f) {
                cur = post_node(GraphNode::Kind::MaskBlurH, cur);
                cur = post_node(GraphNode::Kind::MaskBlurV, cur);
            }
            out = cur;
        }

        // Combine (spec §8): fold another mask in. Self-references and
        // cycles are ignored; the other mask builds through the same memo.
        if (mask.combine_id != 0 && mask.combine_id != mask.id &&
            !building.count(mask.combine_id)) {
            for (size_t mi = 0; mi < doc.masks.size(); ++mi) {
                if (doc.masks[mi].id != mask.combine_id) continue;
                const int other = build_mask(static_cast<int>(mi));
                GraphNode comb;
                comb.kind = GraphNode::Kind::MaskCombine;
                comb.mask_index = mask_index;
                comb.inputs = {out, other};
                out = add_node(std::move(comb));
                break;
            }
        }

        building.erase(mask.id);
        mask_outputs.emplace(mask.id, out);
        return out;
    };

    auto find_mask_index = [&](uint64_t id) -> int {
        if (id == 0) return -1;
        for (size_t i = 0; i < doc.masks.size(); ++i)
            if (doc.masks[i].id == id) return static_cast<int>(i);
        return -1;
    };

    // TRUE GRAPH (docs/flow_canvas.md v3): effect wiring and the final
    // composite come from the link table; legacy chain documents compile
    // through the synthesized equivalent. Adjustment layers stay
    // chain-shaped (their head IS the running composite — inherently
    // sequential), everything else wires freely.
    const std::vector<doc::Document::NodeLink> links =
        doc.links.empty() ? doc::synthesize_links(doc) : doc.links;
    auto link_into = [&](uint64_t to, uint32_t port) -> uint64_t {
        for (const doc::Document::NodeLink& l : links)
            if (l.to == to && l.to_port == port) return l.from;
        return 0;
    };

    // Ownership + activity. Solo mutes within the owner layer (spec §5).
    std::unordered_map<uint64_t, size_t> owner;    // fx id → layer index
    std::unordered_map<uint64_t, const doc::EffectInstance*> fx_by_id;
    std::vector<char> layer_solo(doc.layers.size(), 0);
    for (size_t li = 0; li < doc.layers.size(); ++li)
        for (const doc::EffectInstance& fx : doc.layers[li].stack) {
            owner[fx.id] = li;
            fx_by_id[fx.id] = &fx;
            if (fx.solo && !fx.bypass) layer_solo[li] = 1;
        }
    auto group_bypassed_in = [&](size_t li, uint64_t gid) {
        if (gid == 0) return false;
        for (const doc::Group& g : doc.layers[li].groups)
            if (g.id == gid) return g.bypass;
        return false;
    };
    auto effect_active = [&](uint64_t id) {
        const auto ito = owner.find(id);
        if (ito == owner.end()) return false;
        const size_t li = ito->second;
        const doc::EffectInstance& fx = *fx_by_id[id];
        return doc.layers[li].visible && !fx.bypass &&
               !(layer_solo[li] && !fx.solo) &&
               !group_bypassed_in(li, fx.group_id);
    };

    // Pass A: layer source heads (source/generator + transform).
    std::unordered_map<uint64_t, int> heads;   // layer id → node index
    for (size_t li = 0; li < doc.layers.size(); ++li) {
        const doc::Layer& layer = doc.layers[li];
        if (!layer.visible) continue;
        int cur;
        // Adjustment sources are vestigial (v3): they head at the shared
        // source like an untrimmed clip layer.
        if (layer.source == doc::LayerSourceKind::Clip ||
            layer.source == doc::LayerSourceKind::Adjustment) {
            if (doc::layer_has_trim(layer)) {
                // Trim (spec §5): a private source node the engine feeds
                // from its own reader.
                GraphNode src;
                src.kind = GraphNode::Kind::Source;
                src.layer_index = static_cast<int>(li);
                cur = add_node(std::move(src));
            } else {
                cur = 0;   // the shared decoded-frame source
            }
        } else {
            GraphNode gen;
            gen.kind = GraphNode::Kind::Generator;
            gen.layer_index = static_cast<int>(li);
            cur = add_node(std::move(gen));
        }
        if (doc::layer_has_transform(layer)) {
            GraphNode xf;
            xf.kind = GraphNode::Kind::LayerTransform;
            xf.layer_index = static_cast<int>(li);
            xf.inputs.push_back(cur);
            cur = add_node(std::move(xf));
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

    // Empty-viewport node: used ONLY as graph.output when nothing feeds
    // the composite. No effect ever receives it as an input — an unwired
    // node is DORMANT (not emitted, processes nothing), never fed a
    // fabricated frame.
    int black_node = -1;
    auto black = [&]() {
        if (black_node < 0) {
            GraphNode b;
            b.kind = GraphNode::Kind::Generator;   // layer/mask -1 = black
            black_node = add_node(std::move(b));
        }
        return black_node;
    };

    // One effect emission: matte gating / displace-by-matte exactly as
    // the chain compiler did, with the In node resolved from the links.
    // matte_node (a port-1 IMAGE link — masks ARE images, docs/
    // flow_canvas.md v5.2) wins over the legacy mask_id entity; it runs
    // through the default luma extract so the wired image is the matte.
    std::unordered_map<uint64_t, int> fx_out;   // fx id → output node
    auto emit_one = [&](size_t li, size_t stack_index, int in_node,
                        int aux_node = -1, int matte_node = -1) {
        const doc::EffectInstance& fx = doc.layers[li].stack[stack_index];
        const int mask_index =
            matte_node >= 0 ? -1 : find_mask_index(fx.mask_id);
        int gate = -1;
        if (matte_node >= 0) {
            GraphNode ex;
            ex.kind = GraphNode::Kind::MaskExtract;
            ex.mask_index = -1;   // defaults: plain luma, neutral levels
            ex.inputs.push_back(matte_node);
            gate = add_node(std::move(ex));
        }
        // Displace-by-mask (spec §6.2): the mask grayscale is a second
        // input, not a gate; Time Displace mode 2 consumes it the same
        // way (per-pixel delay map). A wired aux port (v3 N-ports) wins
        // over the mask path — extra stays what the link resolved.
        int extra = aux_node;
        const bool wants_map =
            (fx.type == doc::EffectType::Displace &&
             fx.params.size() > 3 && fx.params[3] >= 0.5f) ||
            (fx.type == doc::EffectType::TimeDisplace &&
             fx.params.size() > 1 && fx.params[1] >= 1.5f);
        if (extra < 0 && wants_map) {
            if (gate >= 0) extra = gate;
            else if (mask_index >= 0) extra = build_mask(mask_index);
        }
        const int fx_node = emit_effect(graph.nodes, fx,
                                        static_cast<int>(li),
                                        static_cast<int>(stack_index), -1,
                                        -1, in_node, flow, extra);
        int out = fx_node;
        // The matte gates the effect UNLESS the matte itself was
        // consumed as the displace map (never double-apply). A wired
        // aux (b/map) does NOT suppress gating — the chain compiler's
        // extra-suppresses-gate rule predates N-ports, when `extra`
        // could only ever BE the mask; kept verbatim it silently ate
        // the matte on any Blend with both b and a matte wired.
        const int gate_out =
            gate >= 0 ? gate
                      : (mask_index >= 0 ? build_mask(mask_index) : -1);
        if (gate_out >= 0 && gate_out != extra) {
            GraphNode apply;
            apply.kind = GraphNode::Kind::MaskApply;
            apply.inputs = {in_node, fx_node, gate_out};
            out = add_node(std::move(apply));
        }
        fx_out[fx.id] = out;
        return out;
    };

    // Pass B: effects in link-topological order. An effect with NO
    // in-wire (or a chain that dangles into nothing) is DORMANT: never
    // emitted, processes nothing, and anything fed only by it starves at
    // the fixpoint and stays dormant too — an unconnected node must not
    // run (v4). Cycle-stuck nodes starve the same way. Aux is optional:
    // phase 0 waits for wired aux producers so their node index exists;
    // phase 1 re-runs treating aux fed by dormant chains as unwired
    // instead of starving the consumer.
    std::vector<std::pair<size_t, size_t>> pending;
    for (size_t li = 0; li < doc.layers.size(); ++li)
        for (size_t i = 0; i < doc.layers[li].stack.size(); ++i) {
            const doc::EffectInstance& fx = doc.layers[li].stack[i];
            if (effect_active(fx.id)) pending.emplace_back(li, i);
        }
    for (int phase = 0; phase < 2; ++phase) {
        bool progress = true;
        while (progress && !pending.empty()) {
            progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                const doc::EffectInstance& fx =
                    doc.layers[it->first].stack[it->second];
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
                    // else: fed by nothing — the port reads as unwired.
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
    // layer mask; adjustment layers emit their (chain-shaped) stacks here,
    // heads bound to the running composite.
    int below = -1;
    for (const doc::Document::NodeLink& l : links) {
        if (l.to != 0 || l.to_port != 0) continue;
        size_t li = SIZE_MAX;
        if (auto ito = owner.find(l.from); ito != owner.end()) {
            li = ito->second;
        } else {
            for (size_t k = 0; k < doc.layers.size(); ++k)
                if (doc.layers[k].id == l.from) li = k;
        }
        if (li == SIZE_MAX || !doc.layers[li].visible) continue;
        const doc::Layer& layer = doc.layers[li];

        // Every contribution resolves through the links — the adjustment
        // special case is gone (docs/flow_canvas.md v3: source in, output
        // out, everything between wires freely; merges are Blend nodes).
        // A dormant chain contributes NOTHING.
        int cur = resolve(l.from);
        if (cur < 0) continue;

        // Layer matte: a port-1 IMAGE link on the layer wins (masks ARE
        // images, v5.2) over the legacy mask_id entity; either gates the
        // whole contribution — the bottom contribution reveals the raw
        // source where the matte is black.
        int gate_out = -1;
        if (const uint64_t lm_link = link_into(layer.id, 1)) {
            const uint64_t lm = effective_from(lm_link);
            int idx = -1;
            if (auto itf = fx_out.find(lm); itf != fx_out.end())
                idx = itf->second;
            else if (auto ith = heads.find(lm); ith != heads.end())
                idx = ith->second;
            if (idx >= 0) {
                GraphNode ex;
                ex.kind = GraphNode::Kind::MaskExtract;
                ex.mask_index = -1;
                ex.inputs.push_back(idx);
                gate_out = add_node(std::move(ex));
            }
        }
        if (gate_out < 0) {
            const int layer_mask = find_mask_index(layer.mask_id);
            if (layer_mask >= 0) gate_out = build_mask(layer_mask);
        }
        if (below < 0) {
            if (gate_out >= 0) {
                GraphNode apply;
                apply.kind = GraphNode::Kind::MaskApply;
                apply.inputs = {0, cur, gate_out};
                cur = add_node(std::move(apply));
            }
            below = cur;
        } else {
            GraphNode blend;
            blend.kind = GraphNode::Kind::LayerBlend;
            blend.layer_index = static_cast<int>(li);
            blend.inputs = {below, cur};
            int blended = add_node(std::move(blend));
            if (gate_out >= 0) {
                GraphNode apply;
                apply.kind = GraphNode::Kind::MaskApply;
                apply.inputs = {below, blended, gate_out};
                blended = add_node(std::move(apply));
            }
            below = blended;
        }
    }
    // Unwired Output (v4 flat graph): nothing feeds the composite, so
    // the composite is BLACK — `output = 0` passed the raw source
    // through, which is chain residue, not a node graph.
    graph.output = below >= 0 ? below : black();

    // Viewport tap (v5.4): the OUTPUT stays the real composite; the big
    // preview publishes graph.preview instead when set. Selection
    // preview first, the mask overlay wins over it. Dormant/boundary/
    // unresolvable selections leave it at -1 (show the output).
    if (preview_node != 0) {
        int idx = -1;
        if (preview_node & doc::kMaskParamBit) {
            const int mi2 =
                find_mask_index(preview_node & ~doc::kMaskParamBit);
            if (mi2 >= 0) idx = build_mask(mi2);
        } else if (auto ith = heads.find(preview_node);
                   ith != heads.end()) {
            idx = ith->second;   // a source node: its transformed head
        } else {
            // Effect — or a group, which previews its bound out member.
            uint64_t id = preview_node;
            for (const doc::Layer& l : doc.layers)
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
        if (idx >= 0 && idx != graph.output) graph.preview = idx;
    }

    // Overlay: show the mask itself (grayscale) in the viewport.
    if (const int overlay_index = find_mask_index(overlay_mask_id);
        overlay_index >= 0) {
        graph.preview = build_mask(overlay_index);
    }

    graph.valid = topo_sort(graph.nodes, graph.order);
    return graph;
}

}  // namespace looks::gfx
