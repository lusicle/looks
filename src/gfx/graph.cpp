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
         fx.params[8] >= 0.5f)) {
        // Quantize joins the flow consumers only in motion-locked dither
        // mode (spec §6.2 lock mode).
        if (flow.node < 0) {
            GraphNode f;
            f.kind = GraphNode::Kind::Flow;
            flow.node = add(std::move(f));
        }
        return add(make(0, {upstream, flow.node}));
    }
    if ((fx.type == doc::EffectType::Displace ||
         fx.type == doc::EffectType::TimeDisplace) &&
        extra_input >= 0)
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
                          uint64_t overlay_mask_id) {
    RenderGraph graph;

    GraphNode source;
    source.kind = GraphNode::Kind::Source;
    graph.nodes.push_back(std::move(source));

    auto add_node = [&](GraphNode node) {
        graph.nodes.push_back(std::move(node));
        return static_cast<int>(graph.nodes.size()) - 1;
    };
    FlowSlot flow;

    // Shared masks compile once; every referencing effect reads the same
    // output node. Recursive (combine references other masks); `building`
    // breaks combine cycles — a mask reached while already on the build
    // stack contributes nothing.
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

    // Layers composite bottom-up (spec §5): each visible layer renders its
    // source through its stack, then blends over the composite below.
    int below = -1;
    for (size_t li = 0; li < doc.layers.size(); ++li) {
        const doc::Layer& layer = doc.layers[li];
        if (!layer.visible) continue;

        int cur;
        if (layer.source == doc::LayerSourceKind::Clip) {
            if (doc::layer_has_trim(layer)) {
                // Trim (spec §5): this layer plays a different clip frame
                // than the playhead — a private source node the engine
                // feeds from its own reader.
                GraphNode src;
                src.kind = GraphNode::Kind::Source;
                src.layer_index = static_cast<int>(li);
                cur = add_node(std::move(src));
            } else {
                cur = 0;   // the shared decoded-frame source
            }
        } else if (layer.source == doc::LayerSourceKind::Adjustment) {
            // Adjustment applies its stack to the composite below; as the
            // bottom layer it just passes the source through.
            cur = below >= 0 ? below : 0;
        } else {
            GraphNode gen;
            gen.kind = GraphNode::Kind::Generator;
            gen.layer_index = static_cast<int>(li);
            cur = add_node(std::move(gen));
        }

        // Transform (spec §5): crop/flip/scale/rotate applied to the layer
        // source before its stack (the rack processes the transformed
        // signal; overlays stay screen-aligned).
        if (doc::layer_has_transform(layer)) {
            GraphNode xf;
            xf.kind = GraphNode::Kind::LayerTransform;
            xf.layer_index = static_cast<int>(li);
            xf.inputs.push_back(cur);
            cur = add_node(std::move(xf));
        }

        auto group_bypassed = [&](uint64_t group_id) {
            if (group_id == 0) return false;
            for (const doc::Group& g : layer.groups)
                if (g.id == group_id) return g.bypass;
            return false;
        };
        // Solo (spec §5): any soloed effect mutes the rest of the stack.
        bool any_solo = false;
        for (const doc::EffectInstance& fx : layer.stack)
            if (fx.solo && !fx.bypass) any_solo = true;
        for (size_t i = 0; i < layer.stack.size(); ++i) {
            if (layer.stack[i].bypass) continue;
            if (any_solo && !layer.stack[i].solo) continue;
            if (group_bypassed(layer.stack[i].group_id)) continue;
            const int mask_index = find_mask_index(layer.stack[i].mask_id);
            // Displace-by-mask (spec §6.2): the mask's grayscale becomes
            // the displacement map (a second effect input), not a gate.
            // Time Displace mode 2 consumes the mask the same way — as
            // its per-pixel delay map.
            int extra = -1;
            const doc::EffectInstance& fx_ref = layer.stack[i];
            if (fx_ref.type == doc::EffectType::Displace &&
                mask_index >= 0 && fx_ref.params.size() > 3 &&
                fx_ref.params[3] >= 0.5f)
                extra = build_mask(mask_index);
            if (fx_ref.type == doc::EffectType::TimeDisplace &&
                mask_index >= 0 && fx_ref.params.size() > 1 &&
                fx_ref.params[1] >= 1.5f)
                extra = build_mask(mask_index);
            const int fx_node = emit_effect(graph.nodes, layer.stack[i],
                                            static_cast<int>(li),
                                            static_cast<int>(i), -1, -1, cur,
                                            flow, extra);
            // A mask consumed as a displacement map does not also gate.
            if (mask_index >= 0 && extra < 0) {
                const int mask_out = build_mask(mask_index);
                GraphNode apply;
                apply.kind = GraphNode::Kind::MaskApply;
                apply.inputs = {cur, fx_node, mask_out};
                cur = add_node(std::move(apply));
            } else {
                cur = fx_node;
            }
        }

        // Layer mask (spec §8: masks attach to layers too): gate the whole
        // layer's contribution. Above the bottom layer the base is the
        // composite below; the bottom layer reveals the raw source where
        // the mask is black (this compositor has no alpha).
        const int layer_mask = find_mask_index(layer.mask_id);

        if (below < 0) {
            if (layer_mask >= 0) {
                GraphNode apply;
                apply.kind = GraphNode::Kind::MaskApply;
                apply.inputs = {0, cur, build_mask(layer_mask)};
                cur = add_node(std::move(apply));
            }
            below = cur;
        } else {
            GraphNode blend;
            blend.kind = GraphNode::Kind::LayerBlend;
            blend.layer_index = static_cast<int>(li);
            blend.inputs = {below, cur};
            int blended = add_node(std::move(blend));
            if (layer_mask >= 0) {
                GraphNode apply;
                apply.kind = GraphNode::Kind::MaskApply;
                apply.inputs = {below, blended, build_mask(layer_mask)};
                blended = add_node(std::move(apply));
            }
            below = blended;
        }
    }
    graph.output = below >= 0 ? below : 0;

    // Overlay: show the mask itself (grayscale) in the viewport.
    if (const int overlay_index = find_mask_index(overlay_mask_id);
        overlay_index >= 0) {
        graph.output = build_mask(overlay_index);
    }

    graph.valid = topo_sort(graph.nodes, graph.order);
    return graph;
}

}  // namespace looks::gfx
