// Project document (spec §5). Milestone-3 shape: one implicit layer holding
// one effect stack. Layers, assets, masks, and the mod matrix land in later
// milestones — but every mutation already goes through doc::Command so undo
// never has to be retrofitted.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doc/effect_instance.h"
#include "doc/masks.h"
#include "doc/modulation.h"

namespace looks::doc {

// Layer source (spec §5): the clip or a generator.
enum class LayerSourceKind : uint32_t {
    Clip = 0,
    Solid,
    Gradient,
    Noise,
    TestPattern,   // 75% color bars + grayscale ramp (spec §5)
    Oscillator,    // video-synth periodic source: bars / rings / plasma
    // LEGACY, load-compat only (docs/flow_canvas.md v4): the flat graph
    // made "adjustment" meaningless — a clip tap merged back through a
    // Blend IS an adjustment. Not creatable anywhere; behaves as Clip.
    Adjustment,
    // The SHAPE NODE (docs/flow_canvas.md v5.2): a centered SDF matte
    // (circle/box/diamond via osc_shape) drawn white-on-black — an
    // ordinary image source meant to run through effects and feed mask
    // anchors. gen_scale = size, gen_angle = feather.
    Shape,
    Count,
};

// Groups (spec §5, v5.3): a Group collapses a sub-stack into one card;
// a saved group IS an "era preset". Membership is a tag on the effect
// (EffectInstance::group_id) — groups don't change render order, only
// the card view, shared bypass, and the exposed face.
struct Group {
    uint64_t id = 0;
    std::string name;
    bool folded = false;
    bool bypass = false;
    // The group FACE (texed expose): member params surfaced on the
    // collapsed card as DIRECT aliases — same value, same command path,
    // no hidden offsets. Motion comes from value nodes wired inside.
    std::vector<ParamKey> exposed;
    // Boundary BINDINGS (v5.3): which member receives the card's In and
    // which feeds its Out. Persistent INTERMEDIARIES — the scoped view's
    // In/Out nodes wire to these regardless of whether anything is
    // connected outside; external link edits route through them and
    // never rewrite the internal picture. 0 = first/last member.
    uint64_t face_in = 0;
    uint64_t face_out = 0;
    // Node-canvas position of the FOLDED group's card (docs/flow_canvas.md
    // v4 subgraphs); (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
    // Scoped-view positions of the In/Out boundary nodes (v5.4: they
    // hold their own place — member drags never tow them). (0,0) =
    // unplaced (derived from the member extent once, then materialized).
    float in_x = 0.0f, in_y = 0.0f;
    float out_x = 0.0f, out_y = 0.0f;
};

struct Layer {
    uint64_t id = 0;
    std::string name;
    LayerSourceKind source = LayerSourceKind::Clip;
    // Generator params: color_a (solid / gradient start), color_b
    // (gradient end), scale (noise cell size), angle (gradient direction).
    float color_a[3] = {0.5f, 0.5f, 0.5f};
    float color_b[3] = {0.1f, 0.1f, 0.1f};
    float gen_scale = 6.0f;
    float gen_angle = 0.0f;
    // Oscillator waveform: 0 sine bars, 1 concentric rings, 2 plasma.
    uint32_t osc_shape = 0;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
    // Layer mask (spec §8: masks are referenced by effects AND layers):
    // gates this layer's whole contribution to the composite. 0 = none.
    uint64_t mask_id = 0;
    // Transform (spec §5: crop/flip/scale/rotate live on the layer, not in
    // stacks). Applied to the layer source BEFORE its stack — the rack
    // processes the transformed signal, so overlays/grids stay screen-
    // aligned. Above the bottom layer the blend gates to the transformed
    // region, so crop/scale-down reveal the composite below.
    float crop_l = 0.0f, crop_r = 0.0f;   // fraction of frame, 0..0.45
    float crop_t = 0.0f, crop_b = 0.0f;
    bool flip_h = false, flip_v = false;
    float xf_scale = 1.0f;                // about frame center, 0.25..4
    float xf_rotate = 0.0f;               // degrees, -180..180
    // Trim (spec §5): the segment of the source clip this layer plays,
    // frames [in, out). out 0 = clip end. The layer holds its last trimmed
    // frame past the segment (never blanks a looping preview).
    uint32_t trim_in = 0;
    uint32_t trim_out = 0;
    // Node-canvas position of the layer's SOURCE node (docs/
    // flow_canvas.md); (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
    std::vector<EffectInstance> stack;
    std::vector<Group> groups;
};

inline bool layer_has_transform(const Layer& l) {
    return l.crop_l > 0.0f || l.crop_r > 0.0f || l.crop_t > 0.0f ||
           l.crop_b > 0.0f || l.flip_h || l.flip_v ||
           l.xf_scale != 1.0f || l.xf_rotate != 0.0f;
}

inline bool layer_has_trim(const Layer& l) {
    return l.source == LayerSourceKind::Clip &&
           (l.trim_in > 0 || l.trim_out > 0);
}

// TRUE GRAPH (docs/flow_canvas.md v3): layers are storage bags + source
// nodes, not a composite hierarchy — branches merge through Blend nodes.
// The old 3-layer cap is gone; this bounds runaway documents only.
inline constexpr size_t kMaxLayers = 16;
inline constexpr float kMaxSpeed = 4.0f;  // time-remap speed range 0..4

struct Document {
    std::string name = "untitled";
    // Source clip for the base layer (empty = test pattern). The asset list
    // proper arrives with multi-asset support; v1 is one clip per project.
    std::string clip_path;
    uint64_t master_seed = 0;
    // Frame render cache RAM budget in MiB (spec §10: default 2 GB,
    // configurable); 0 disables caching for this project.
    uint32_t cache_mb = 2048;
    // Half-res proxy toggle (spec §3/§10): preview decodes <stem>.proxy.mez
    // when present. Export always renders full-res.
    bool use_proxy = false;
    // Bumped on every executed/undone/redone command. Cheap "did anything
    // change" signal for autosave and dirty-flag UI; render caching keys on
    // per-subgraph hashes later, not on this.
    uint64_t revision = 0;

    // Bottom-up layer list (layers[0] is the base). Every document starts
    // with one clip layer.
    std::vector<Layer> layers;
    // Monotonic id source for stable identity (effects, layers, masks)
    // across reorder/undo.
    uint64_t next_effect_id = 1;

    Document() {
        Layer base;
        base.id = next_effect_id++;
        base.name = "layer 1";
        layers.push_back(std::move(base));
    }

    // Modulation (spec §7): global mod matrix + per-param keyframe lanes +
    // A/B/C snapshot slots.
    std::vector<ModRoute> mod_routes;
    std::vector<KeyframeLane> lanes;
    uint64_t next_route_id = 1;
    Snapshot snapshots[3];

    // Morph (spec §7): timed interpolation between two snapshot slots; the
    // position is itself a mod target (ParamKey {0, 0} = "global.morph").
    // Active only when both slots hold valid snapshots.
    int morph_from = 0;
    int morph_to = 1;
    float morph_pos = 0.0f;

    // Time remap (spec §6.1: speed ramp / reverse / ping-pong). Speed is a
    // mod target (ParamKey {0, 1} = "global.speed") so lanes ramp it; the
    // playback position is the prefix sum of per-frame speed (mod/eval.h).
    float speed = 1.0f;
    uint32_t time_mode = 0;   // 0 forward, 1 reverse, 2 ping-pong

    // Timeline region (spec §3/§9): clip trim [in, out) — out 0 = clip end
    // — edited by the ruler's trim handles; export renders the trim. The
    // loop region (0/0 = off) confines looping playback inside the trim.
    uint32_t clip_trim_in = 0;
    uint32_t clip_trim_out = 0;
    uint32_t loop_in = 0;
    uint32_t loop_out = 0;
    // Still-image clip length in frames (0 = as imported). The bundle on
    // the scratch cache is regenerable, so the one user decision baked
    // into it must live here; re-applied whenever the clip opens.
    uint32_t still_duration_frames = 0;

    // Sidechain (spec §7): analyze an external WAV or another MP4's audio
    // instead of the clip's own; the audio-derived mod curves come from it
    // while video curves stay with the clip. sidechain_mux muxes its audio
    // into the export instead of the clip's. audio_offset_ms nudges audio
    // against video everywhere (curve sampling, monitoring, export).
    std::string sidechain_path;
    bool sidechain_mux = false;
    float audio_offset_ms = 0.0f;

    // Masks (spec §8): first-class named objects referenced by effects.
    std::vector<Mask> masks;
    uint64_t next_mask_id = 1;

    // Node-canvas position of the Output node (docs/flow_canvas.md);
    // (0,0) = unplaced.
    float out_node_x = 0.0f;
    float out_node_y = 0.0f;

    // TRUE GRAPH (docs/flow_canvas.md v3): first-class links between node
    // outputs and named input ports. Node ids: effect / layer / mask ids
    // (one counter space per kind, disambiguated by the consumer); 0 as
    // `to` = the Output node. to_port 0 = In, 1 = Mask, 2+ = per-effect
    // aux inputs (Warp/Displace/B...). While `links` is empty the loader
    // and engine synthesize links from the legacy per-layer stack order —
    // every old project and preset opens unchanged.
    struct NodeLink {
        uint64_t from = 0;
        uint64_t to = 0;
        uint32_t to_port = 0;
    };
    std::vector<NodeLink> links;

    // Canvas frames (docs/flow_canvas.md v3): titled visual grouping
    // boxes, texed-style. Pure annotation — nothing reads them but the
    // canvas. Ids come from next_effect_id.
    struct Frame {
        uint64_t id = 0;
        float x = 0.0f, y = 0.0f;
        float w = 480.0f, h = 360.0f;
        std::string title;
        // Colour tag (texed frame colour): 0 = none, 1..8 = palette hue.
        uint32_t color = 0;
    };
    std::vector<Frame> frames;
};

// The legacy-chain topology as links: source → effects in stack order →
// Output per layer. Mask wiring is NOT in the table — fx.mask_id /
// layer.mask_id stay the single source of truth (the canvas draws those
// wires from the fields, the compiler reads them directly).
inline std::vector<Document::NodeLink> synthesize_links(const Document& d) {
    std::vector<Document::NodeLink> links;
    for (const Layer& layer : d.layers) {
        uint64_t prev = layer.id;
        for (const EffectInstance& fx : layer.stack) {
            links.push_back({prev, fx.id, 0});
            prev = fx.id;
        }
        links.push_back({prev, 0, 0});
    }
    return links;
}

// Materializes the synthesized links onto a legacy document (first link
// edit, canvas display). Idempotent when links already exist.
inline void ensure_links(Document& d) {
    if (d.links.empty()) d.links = synthesize_links(d);
}

}  // namespace looks::doc
