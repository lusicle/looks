// Project document: assets (imported media), looks, and sequences.
// Every mutation goes through doc::Command so undo never has to be
// retrofitted.
//
// Two entities, each pure:
// - A LOOK is a TIMELESS node graph with one linear local clock. Its
//   sources (media, generators, nested looks, nested sequences) play in
//   LOCKSTEP with that clock, 1:1 - no scheduling inside looks. A media
//   node carries one static SLIP (media in-point) so two media nodes can hold
//   a fixed sync offset; that is a parameter, not a schedule. Effects
//   exist ONLY in look graphs. Keyframe lanes are look-local, keyed on
//   the local clock, so motion travels with the look.
// - A SEQUENCE is arrangement and nothing else: video lanes of
//   placements stacked with plain alpha-over (top lane over bottom),
//   plus audio tracks. It owns NO effects at any depth - that is the
//   line that keeps razor identity universal (nothing at sequence level
//   has state, so cutting a block and butting the halves is always
//   bit-identical). Placements target looks or sequences; raw media
//   never sits on a timeline - importing wraps the media in a look.
// Both entities are TEMPLATES shared by reference: params live on the
// entity, per-instance state (effect history) keys on the instance path
// in the engine, MAKE UNIQUE is the explicit fork.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doc/effect_instance.h"
#include "doc/modulation.h"

namespace looks::doc {

// Imported media bundle: <stem>.mez plus its .pcm/.analysis/.proxy
// sidecars. Pure media - scheduling belongs to the sequences referencing
// the look that wraps it.
struct Asset {
    uint64_t id = 0;
    std::string name;      // display name (the file stem)
    std::string path;      // mezzanine path, as opened by the player
    // Cached from the bundle so durations and the canvas size resolve
    // before any decode opens; refreshed whenever the asset opens.
    uint32_t frame_count = 0;
    double fps = 0.0;
    uint32_t width = 0, height = 0;
    // Still-image length in frames (0 = as imported). The bundle on the
    // scratch cache is regenerable, so the one user decision baked into it
    // lives here and is re-applied whenever the asset opens.
    uint32_t still_duration_frames = 0;
    // TRUE STILL (png/tga import): its frame count is authored on the
    // PROJECT clock (the duration entry), so the conform rate never
    // applies - unlike audio cover art, whose image side is sized in
    // media frames to span the audio. Refreshed with the other facts.
    bool still = false;
    // Browser bin this asset files under; 0 = the project root.
    uint64_t bin = 0;
};

// Browser BIN: pure project organisation, like an NLE's project-panel
// folders. Bins nest by parent (0 = root) and hold nothing themselves -
// membership is the `bin` field on looks, sequences and assets, so
// deleting a bin never deletes content. Nothing outside the browser
// reads them.
struct Bin {
    uint64_t id = 0;
    std::string name;
    uint64_t parent = 0;
};

// Where a block sits on a sequence's timeline, and WHAT plays there: a
// look or another sequence (never raw media). Local frame to target frame
// is affine - target = (local - t_in) * speed + source_in - so nesting
// composes in closed form and stays deterministic.
struct Placement {
    uint64_t id = 0;         // document-unique, minted like node ids
    // The look or sequence this block plays; 0 = unbound (dormant).
    // Looks, sequences and assets share one id space, so the document
    // resolves which kind by lookup.
    uint64_t target = 0;
    uint32_t t_in = 0;       // local frame the block starts at
    uint32_t t_out = 0;      // exclusive local end; 0 = run to target end
    uint32_t source_in = 0;  // target-local frame shown at t_in
    float speed = 1.0f;      // constant per placement
    // LINK GROUP: the video and audio placements one drop laid down share
    // a group id, and every timing edit applies to the whole group -
    // unlink (clearing this) is the one deliberate desync, which is what
    // a J or L cut is. 0 = unlinked.
    uint64_t link = 0;
    // Per-placement level in the mix; scales everything nested under the
    // target. Meaningful on AUDIO placements.
    float audio_gain = 1.0f;   // 0..2, linear
    bool audio_mute = false;
    // Canvas composition (Motion, per block, VIDEO lanes): offset in
    // canvas fractions (+x right, +y down; 0 = centered), uniform scale
    // and rotation (degrees) about the ANCHOR, opacity into the lane
    // stack. Pure stateless geometry - razored halves inherit it
    // bit-identically and sequences still own no effects.
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float scale = 1.0f;
    float rotate = 0.0f;
    float opacity = 1.0f;
    // Scale/rotate pivot in BLOCK-LOCAL canvas fractions (travels with
    // the block): forward map out = a + shift + S*R*(src - a), so 0.5
    // is the media centre and the anchor alone never moves a pixel.
    float anchor_x = 0.5f;
    float anchor_y = 0.5f;
};

inline bool placement_has_transform(const Placement& p) {
    return p.pos_x != 0.0f || p.pos_y != 0.0f || p.scale != 1.0f ||
           p.rotate != 0.0f;
}

// A sequence VIDEO LANE: placements in local time, topmost lane
// composites last. Pure arrangement - it owns no effects, no state.
// hidden drops the lane from the composite (render and export alike -
// document state, not a view toggle); lock is an edit guard the
// gesture surfaces honor - neither is an effect, both are arrangement.
struct SeqTrack {
    uint64_t id = 0;
    std::string name;
    std::vector<Placement> placements;
    bool hidden = false;
    bool lock = false;
};

// AUDIO TRACK: sequence structure that feeds the mix, never an image.
// Summing commutes, so audio track order is display only. Gain and mute
// here scale every placement on the track; together with the
// per-placement pair they are the whole mixer.
struct AudioTrack {
    uint64_t id = 0;
    std::string name;
    std::vector<Placement> placements;
    float gain = 1.0f;   // 0..2, linear
    bool mute = false;
    bool lock = false;   // edit guard only; the mix ignores it
};

// Layer source: what a look-graph source node IS. Media reads an imported asset in
// lockstep (plus slip); generators have no media; LookRef/SequenceRef
// nest an entity as a lockstep source - a mask IS a nested look.
enum class LayerSourceKind : uint32_t {
    // MEDIA, not "clip": the layer reads an imported asset - video,
    // still, or audio-bearing - in lockstep with the look's clock.
    Media = 0,
    // Generator numeric values are baked into gen.comp.slang's kind
    // switch - renumbering means editing the shader in the same change.
    Solid,
    Gradient,
    Noise,
    TestPattern,   // 75% color bars + grayscale ramp
    Oscillator,    // video-synth periodic source: bars / rings / plasma
    // The SHAPE NODE: a centered SDF matte
    // (circle/box/diamond via osc_shape) drawn as premultiplied coverage
    // - an ordinary image source meant to run through effects and feed
    // mask anchors. gen_scale = size, gen_angle = feather.
    Shape,
    // Nested entities play in LOCKSTEP with this look's clock (1:1, no
    // per-source timing - the sequence placing THIS look owns all
    // scheduling). SequenceRef is the only way to put effects over an
    // edit: grade a cut by wrapping the sequence in a look.
    LookRef,
    SequenceRef,
    Count,
};

// Graph wiring: an output feeding a named input port. Node ids are
// effect / layer ids; 0 as `to` = the look's Output node. to_port 0 = In,
// 1 = the matte port (the wired image gates the consumer through luma
// extract), 2+ = per-effect aux inputs (Warp/Displace/B...). On the
// OUTPUT node port 1 is the split-mode audio-in instead (Look
// .audio_split) - the Output has no matte. While a look's links are
// empty the loader and engine synthesize them from stack order.
struct NodeLink {
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t to_port = 0;
};

// Canvas frames: titled visual grouping boxes,
// texed-style. Pure annotation - nothing reads them but the canvas.
struct CanvasFrame {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f;
    float w = 480.0f, h = 360.0f;
    std::string title;
    // Colour tag (texed frame colour): 0 = none, 1..8 = palette hue.
    uint32_t color = 0;
};

// Groups: a Group collapses a sub-stack into one card;
// a saved group IS an "era preset". Membership is a tag on the effect
// (EffectInstance::group_id) - groups don't change render order, only
// the card view, shared bypass, and the exposed face.
struct Group {
    uint64_t id = 0;
    std::string name;
    bool folded = false;
    bool bypass = false;
    // The group FACE (texed expose): member params surfaced on the
    // collapsed card as DIRECT aliases - same value, same command path,
    // no hidden offsets. Motion comes from value nodes wired inside.
    std::vector<ParamKey> exposed;
    // Boundary BINDINGS: which member receives the card's In and
    // which feeds its Out. Persistent INTERMEDIARIES - the scoped view's
    // In/Out nodes wire to these regardless of whether anything is
    // connected outside; external link edits route through them and
    // never rewrite the internal picture. 0 = first/last member.
    uint64_t face_in = 0;
    uint64_t face_out = 0;
    // Node-canvas position of the FOLDED group's card; (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
    // Scoped-view positions of the In/Out boundary nodes (they
    // hold their own place - member drags never tow them). (0,0) =
    // unplaced (derived from the member extent once, then materialized).
    float in_x = 0.0f, in_y = 0.0f;
    float out_x = 0.0f, out_y = 0.0f;
};

// One custom-shape control point, canvas fractions: the anchor plus two
// cubic tangent handles stored as OFFSETS from it (zero offsets = a
// corner point). The shape node rasterizes the cubic through these into
// an SDF (gfx/shape_sdf); the monitor path editor drags them.
struct PathPoint {
    float ax = 0.0f, ay = 0.0f;
    float in_dx = 0.0f, in_dy = 0.0f;
    float out_dx = 0.0f, out_dy = 0.0f;
};

struct Layer {
    uint64_t id = 0;
    std::string name;
    LayerSourceKind source = LayerSourceKind::Media;
    // Media: the asset this node reads, in lockstep with the look's
    // clock. slip is the one timing nuance a look allows: a static media
    // in-point (local frame 0 reads media frame `slip`) so two sources
    // can hold a fixed sync offset. A parameter, not a schedule.
    uint64_t asset = 0;
    uint32_t slip = 0;
    // TIMELINE LOCK (media only): the node reads the asset at the ROOT
    // timeline frame instead of the look clock, so every placement of
    // the look reads identical media positions - offset blocks stay in
    // sync (and their reactive signals with them). Closed-form and
    // razor-invariant; placement speed/source_in stop applying to this
    // node; slip stays the manual nudge. At look scope root = local, so
    // the toggle degrades to lockstep.
    bool timeline_lock = false;
    // LookRef/SequenceRef: the nested entity, playing 1:1.
    uint64_t target = 0;
    // Generator params: color_a (solid / gradient start), color_b
    // (gradient end), scale (noise cell size), angle (gradient direction).
    float color_a[3] = {0.5f, 0.5f, 0.5f};
    float color_b[3] = {0.1f, 0.1f, 0.1f};
    float gen_scale = 6.0f;
    float gen_angle = 0.0f;
    // Oscillator phase, percent of one period: the pattern slides along
    // its own direction and wraps (0 and 100 are identical), so typed
    // and keyframed values may run past 100 for long loops.
    float gen_phase = 0.0f;
    // Oscillator waveform: 0 sine bars, 1 concentric rings, 2 plasma.
    uint32_t osc_shape = 0;
    // Custom shape path (shape node, osc_shape 3): closed = a filled
    // matte, open = a feathered stroke. Empty custom draws nothing.
    std::vector<PathPoint> path;
    bool path_closed = true;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
    // Transform (crop/flip/scale/rotate live on the layer, not in
    // stacks). Applied to the layer source BEFORE its stack - the rack
    // processes the transformed signal, so overlays/grids stay screen-
    // aligned. Coverage is written as premultiplied alpha, so crop/
    // scale-down reveal the composite below through whatever the stack
    // did to the layer's shape.
    float crop_l = 0.0f, crop_r = 0.0f;   // fraction of frame, 0..0.45
    float crop_t = 0.0f, crop_b = 0.0f;
    bool flip_h = false, flip_v = false;
    float xf_scale = 1.0f;                // about the anchor, 0.25..4
    float xf_rotate = 0.0f;               // degrees, -180..180
    // Scale/rotate pivot in frame fractions; 0.5 = frame centre (the
    // anchor alone never moves a pixel).
    float xf_anchor_x = 0.5f;
    float xf_anchor_y = 0.5f;
    // Node-canvas position of the layer's SOURCE node; (0,0) = unplaced.
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

inline bool layer_is_media(const Layer& l) {
    return l.source == LayerSourceKind::Media;
}
inline bool layer_is_nested(const Layer& l) {
    return l.source == LayerSourceKind::LookRef ||
           l.source == LayerSourceKind::SequenceRef;
}

// TRUE GRAPH: layers are storage bags + source
// nodes, not a composite hierarchy - branches merge through Blend nodes.
// These bound runaway documents only. kMaxLayers bounds GRAPH NODES, not
// edit length - a sequence lane holds up to kMaxPlacementsPerTrack cuts.
inline constexpr size_t kMaxLayers = 16;
inline constexpr size_t kMaxPlacementsPerTrack = 256;
inline constexpr size_t kMaxLooks = 256;
inline constexpr int kMaxLookDepth = 8;   // nesting guard, both entities
inline constexpr float kMaxSpeed = 4.0f;  // time-remap speed range 0..4

// Optional per-entity FORMAT: all-zero = UNSET = inherit the PROJECT
// format, so an unset entity behaves identically wherever it nests.
// fps pins the entity's LOCAL CLOCK (lanes, value graph, stateful
// effects all tick it; nesting hops conform via the composed rate);
// w/h pin its render canvas (the consumer fits it like media).
// Formats live on the TEMPLATE, shared by reference - MAKE UNIQUE
// forks them; placements never carry one.
struct EntityFormat {
    uint32_t w = 0, h = 0;
    double fps = 0.0;
};

inline bool format_has_fps(const EntityFormat& f) { return f.fps > 0.0; }
inline bool format_has_canvas(const EntityFormat& f) {
    return f.w > 0 && f.h > 0;
}

// A look: one graph, one linear local clock, no arrangement.
struct Look {
    uint64_t id = 0;
    std::string name;
    // Local length in frames; 0 = derived from the longest source
    // (look_duration). Generator-only looks derive 0 = unbounded.
    uint32_t duration = 0;
    EntityFormat format;
    // Browser bin; 0 = the project root.
    uint64_t bin = 0;
    // Output audio routing. COMBINED (false, default): image and audio
    // arrive on the one In wire - the voice is the port-0 chain's audio,
    // so looks wire exactly as always and the bottom chain wins the
    // fan-in. SPLIT (true): the Output grows a dedicated audio-in
    // (to=0, to_port=1) for a voice that differs from the picture's
    // chain; unwired = silent, not fallback.
    bool audio_split = false;

    std::vector<Layer> layers;
    std::vector<NodeLink> links;
    std::vector<CanvasFrame> frames;

    // Modulation is look-local: lanes key on THIS look's frames, and a
    // saved look carries its own motion with it. value_nodes + mod_routes
    // are the value graph (modulation.h): shared signal nodes wired onto
    // params, acyclic node-to-node.
    std::vector<ValueNode> value_nodes;
    std::vector<ModRoute> mod_routes;
    std::vector<KeyframeLane> lanes;
    Snapshot snapshots[3];
    // Morph: timed interpolation between two snapshot slots; the
    // position is itself a mod target (ParamKey {0, 0} = "global.morph").
    // Active only when both slots hold valid snapshots.
    int morph_from = 0;
    int morph_to = 1;
    float morph_pos = 0.0f;

    // Node-canvas position of this look's Output node; (0,0) = unplaced.
    float out_node_x = 0.0f;
    float out_node_y = 0.0f;
};

// A sequence: arrangement and nothing else. tracks[0] is the bottom
// lane; the topmost lane composites last, plain alpha-over - no blend
// modes, no masks, no effects at this level, at any depth.
struct Sequence {
    uint64_t id = 0;
    std::string name;
    // Local length in frames; 0 = derived from the furthest block end.
    uint32_t duration = 0;
    EntityFormat format;
    // Browser bin; 0 = the project root.
    uint64_t bin = 0;

    std::vector<SeqTrack> tracks;
    std::vector<AudioTrack> audio;

    // Timeline region: trim [in, out) is what plays and exports (out 0 =
    // the sequence's end), the loop region confines looping playback
    // inside it (0/0 = off), and markers are plain frame bookmarks.
    uint32_t trim_in = 0;
    uint32_t trim_out = 0;
    uint32_t loop_in = 0;
    uint32_t loop_out = 0;
    std::vector<uint32_t> markers;
};

struct Document {
    std::string name = "untitled";
    // Imported media. Media layers bind to these by id.
    std::vector<Asset> assets;
    // Every look and sequence in the project; root_sequence is the
    // project timeline - the one export renders by default. It is only
    // a default: sequences are ordinary nestable entities.
    std::vector<Look> looks;
    std::vector<Sequence> sequences;
    uint64_t root_sequence = 0;
    // Browser organisation only; nothing renders or plays from these.
    std::vector<Bin> bins;

    uint64_t master_seed = 0;
    // Project frame rate: one clock for every entity, so nested local
    // times stay commensurable. 0 = derive from the first asset.
    double fps = 0.0;
    // Project canvas: what everything renders into. 0/0 = derive from the
    // first bound asset. The timeline owns the format - the media under the
    // playhead must not decide the working resolution, or a cut between
    // two sizes would resize the whole graph mid-playback.
    uint32_t canvas_w = 0, canvas_h = 0;
    // Frame render cache RAM budget in MiB (default 2 GB,
    // configurable); 0 disables caching for this project.
    uint32_t cache_mb = 2048;
    // Half-res proxy toggle: preview decodes <stem>.proxy.mez
    // when present. Export always renders full-res.
    bool use_proxy = false;
    // Bumped on every executed/undone/redone command. Cheap "did anything
    // change" signal for autosave and dirty-flag UI; render caching keys on
    // per-subgraph hashes later, not on this.
    uint64_t revision = 0;

    // Monotonic id source for stable identity (effects, layers, groups,
    // looks, sequences, assets, placements) across reorder/undo. One
    // counter so an id is unique document-wide, which is what lets
    // links, mod targets, placement targets and instance paths name
    // things without a scope prefix.
    uint64_t next_effect_id = 1;
    uint64_t next_route_id = 1;

    // Time remap (speed / reverse / ping-pong) on the ROOT sequence's
    // playback. Sequences have no keyframes (keyframing is look-local),
    // so this is a scalar, not a lane target.
    float speed = 1.0f;
    uint32_t time_mode = 0;   // 0 forward, 1 reverse, 2 ping-pong

    // Sidechain: analyze an external WAV or another MP4's audio
    // instead of the media's own; the audio-derived mod curves come from it
    // while video curves stay with the media. sidechain_mux muxes its audio
    // into the export instead of the media's. audio_offset_ms nudges audio
    // against video everywhere (curve sampling, monitoring, export).
    std::string sidechain_path;
    bool sidechain_mux = false;
    float audio_offset_ms = 0.0f;

    // Export settings: bitrate, output scale (1 = source size,
    // 2/4 = half/quarter through the same proxy path preview uses), and
    // an audio mute. Project state - a deliverable spec, not UI chrome.
    float export_bitrate_mbps = 8.0f;
    uint32_t export_scale = 1;
    bool export_audio = true;

    // A fresh project holds one empty sequence (one video lane, one
    // audio track) and one starter look with a single media node - the
    // timeline to cut on and a look to build in.
    Document() {
        Sequence seq;
        seq.id = next_effect_id++;
        seq.name = "sequence 1";
        SeqTrack lane;
        lane.id = next_effect_id++;
        lane.name = "v1";
        seq.tracks.push_back(std::move(lane));
        AudioTrack atrack;
        atrack.id = next_effect_id++;
        atrack.name = "a1";
        seq.audio.push_back(std::move(atrack));
        root_sequence = seq.id;
        sequences.push_back(std::move(seq));

        Look look;
        look.id = next_effect_id++;
        look.name = "look 1";
        Layer base;
        base.id = next_effect_id++;
        base.name = "layer 1";
        look.layers.push_back(std::move(base));
        looks.push_back(std::move(look));
    }

    Look* find_look(uint64_t id) {
        for (Look& l : looks)
            if (l.id == id) return &l;
        return nullptr;
    }
    const Look* find_look(uint64_t id) const {
        for (const Look& l : looks)
            if (l.id == id) return &l;
        return nullptr;
    }
    Sequence* find_sequence(uint64_t id) {
        for (Sequence& s : sequences)
            if (s.id == id) return &s;
        return nullptr;
    }
    const Sequence* find_sequence(uint64_t id) const {
        for (const Sequence& s : sequences)
            if (s.id == id) return &s;
        return nullptr;
    }
    Bin* find_bin(uint64_t id) {
        for (Bin& b : bins)
            if (b.id == id) return &b;
        return nullptr;
    }
    const Bin* find_bin(uint64_t id) const {
        for (const Bin& b : bins)
            if (b.id == id) return &b;
        return nullptr;
    }
    // For callers holding an id they know is live (commands capture the
    // entity they target, and undo order keeps it alive). UI code that
    // may hold a stale id uses find_* and handles null. The document
    // always holds at least one look and one sequence.
    Look& look(uint64_t id) {
        Look* l = find_look(id);
        return l ? *l : looks.front();
    }
    const Look& look(uint64_t id) const {
        const Look* l = find_look(id);
        return l ? *l : looks.front();
    }
    Sequence& sequence(uint64_t id) {
        Sequence* s = find_sequence(id);
        return s ? *s : sequences.front();
    }
    const Sequence& sequence(uint64_t id) const {
        const Sequence* s = find_sequence(id);
        return s ? *s : sequences.front();
    }
    Sequence& root() { return sequence(root_sequence); }
    const Sequence& root() const { return sequence(root_sequence); }

    Asset* find_asset(uint64_t id) {
        for (Asset& a : assets)
            if (a.id == id) return &a;
        return nullptr;
    }
    const Asset* find_asset(uint64_t id) const {
        for (const Asset& a : assets)
            if (a.id == id) return &a;
        return nullptr;
    }
    // The media file the single-asset paths still key on (player, analysis,
    // export mux) until every consumer walks the flatten.
    const Asset* primary_asset() const {
        return assets.empty() ? nullptr : &assets.front();
    }
};

// The project's working resolution: the explicit canvas, else the first
// asset that knows its own size, else a 1080p default. Even dimensions -
// the NV12/codec paths require them.
inline void canvas_size(const Document& doc, uint32_t* w, uint32_t* h) {
    uint32_t cw = doc.canvas_w, cvh = doc.canvas_h;
    if (!cw || !cvh) {
        cw = 0;
        cvh = 0;
        for (const Asset& a : doc.assets)
            if (a.width && a.height) {
                cw = a.width;
                cvh = a.height;
                break;
            }
    }
    if (!cw || !cvh) {
        cw = 1920;
        cvh = 1080;
    }
    *w = std::max(cw & ~1u, 2u);
    *h = std::max(cvh & ~1u, 2u);
}

inline uint32_t look_duration(const Document& doc, const Look& look,
                              int depth);
inline uint32_t sequence_duration(const Document& doc, const Sequence& seq,
                                  int depth);

// The project's frame rate: its own setting, else the first asset that
// knows one (asset fps is cached from the bundle, so this resolves
// before any decode opens). One clock for every entity, so nested local
// times stay commensurable.
inline double project_fps(const Document& doc) {
    if (doc.fps > 0.0) return doc.fps;
    for (const Asset& a : doc.assets)
        if (a.fps > 0.0) return a.fps;
    return 30.0;
}

// The clock an entity's LOCAL frames tick at: its pinned fps, else the
// project's. One conversion rule everywhere: a nesting hop's ratio is
// child_effective_fps / parent_effective_fps (frames of child per
// frame of parent).
inline double effective_fps(const Document& doc, const Look& l) {
    return format_has_fps(l.format) ? l.format.fps : project_fps(doc);
}
inline double effective_fps(const Document& doc, const Sequence& s) {
    return format_has_fps(s.format) ? s.format.fps : project_fps(doc);
}
inline double entity_fps(const Document& doc, uint64_t id) {
    if (const Look* l = doc.find_look(id)) return effective_fps(doc, *l);
    if (const Sequence* s = doc.find_sequence(id))
        return effective_fps(doc, *s);
    return project_fps(doc);
}

// Media-hop conform ratio: MEDIA frames advanced per CLOCK frame (the
// OWNING entity's effective rate), so a mismatched-rate asset holds
// its true duration instead of playing fast or slow (audio already
// conforms by sample rate - this is the picture's half). True stills
// and unknown rates play 1:1 by frame.
inline double media_conform_rate(const Document& doc, const Asset& a,
                                 double clock_fps) {
    (void)doc;
    if (a.still) return 1.0;
    return (a.fps > 0.0 && clock_fps > 0.0) ? a.fps / clock_fps : 1.0;
}

// Entity-local frames -> CLOCK frames across a hop (ratio = child
// frames per clock frame); ceil keeps the last partial frame playable.
inline uint32_t conform_frames(uint32_t frames, double ratio) {
    if (frames == 0 || ratio <= 0.0) return frames;
    return static_cast<uint32_t>(
        std::ceil(static_cast<double>(frames) / ratio));
}

// Frame count a look-graph source can play from local 0, in the OWNING
// look's clock frames (clock_fps): the media past its slip for media
// (conformed onto the clock), the nested entity's duration for refs
// (conformed through the hop ratio), 0 for generators and unbound
// sources (no when / unbounded).
inline uint32_t layer_source_length(const Document& doc, const Layer& l,
                                    double clock_fps, int depth = 0) {
    if (depth >= kMaxLookDepth) return 0;
    if (layer_is_media(l)) {
        const Asset* a = doc.find_asset(l.asset);
        if (!a || !a->frame_count) return 0;
        const uint32_t remain =
            a->frame_count > l.slip ? a->frame_count - l.slip : 0;
        return conform_frames(remain, media_conform_rate(doc, *a,
                                                         clock_fps));
    }
    if (l.source == LayerSourceKind::LookRef) {
        const Look* t = doc.find_look(l.target);
        if (!t) return 0;
        return conform_frames(look_duration(doc, *t, depth + 1),
                              effective_fps(doc, *t) / clock_fps);
    }
    if (l.source == LayerSourceKind::SequenceRef) {
        const Sequence* t = doc.find_sequence(l.target);
        if (!t) return 0;
        return conform_frames(sequence_duration(doc, *t, depth + 1),
                              effective_fps(doc, *t) / clock_fps);
    }
    return 0;
}

// Local length of a look IN ITS OWN clock frames: its explicit
// duration, else its longest source (everything plays in lockstep from
// local 0). Generator-only looks derive 0 = unbounded - the placing
// block bounds them.
inline uint32_t look_duration(const Document& doc, const Look& look,
                              int depth = 0) {
    if (look.duration) return look.duration;
    if (depth >= kMaxLookDepth) return 0;
    const double eff = effective_fps(doc, look);
    uint32_t end = 0;
    for (const Layer& l : look.layers)
        end = std::max(end, layer_source_length(doc, l, eff, depth));
    return end;
}

// Frame count of what a placement plays, in the TARGET entity's OWN
// clock frames (source_in and the ratio-aware end math live in that
// domain); 0 for unbound targets (unbounded).
inline uint32_t source_length(const Document& doc, const Placement& p,
                              int depth = 0) {
    if (!p.target || depth >= kMaxLookDepth) return 0;
    if (const Look* l = doc.find_look(p.target))
        return look_duration(doc, *l, depth);
    if (const Sequence* s = doc.find_sequence(p.target))
        return sequence_duration(doc, *s, depth);
    return 0;
}

// The nesting hop's clock ratio for a placement: TARGET frames per
// PARENT frame (1 for unbound targets and matched clocks). Composes
// with the placement speed - the affine's child advance per parent
// frame is speed * ratio.
inline double placement_ratio(const Document& doc, const Placement& p,
                              double parent_fps) {
    if (!p.target || parent_fps <= 0.0) return 1.0;
    return entity_fps(doc, p.target) / parent_fps;
}

// Exclusive local end of a placement: its explicit out point, else what
// REMAINS of the target (past source_in, through speed * hop ratio)
// from t_in. source_len and source_in are TARGET frames; ratio converts
// the remainder onto the parent clock. 0 = unbounded.
inline uint32_t placement_end(const Placement& p, uint32_t source_len,
                              double ratio = 1.0) {
    if (p.t_out) return p.t_out;
    if (!source_len) return 0;
    // What remains past source_in: a trimmed or razored placement ends
    // when its media does, not a freeze-frame later.
    const uint32_t remain =
        source_len > p.source_in ? source_len - p.source_in : 0;
    if (!remain) return p.t_in + 1;   // degenerate: one black-ish frame
    // A FROZEN placement (speed 0) would run forever; cap the span so the
    // result stays a frame index instead of wrapping. kFrameCeiling is
    // far past any real timeline and well inside uint32.
    constexpr double kFrameCeiling = 1.0e9;
    const double advance = std::max(
        static_cast<double>(p.speed) * (ratio > 0.0 ? ratio : 1.0), 1e-6);
    const double span = std::min(static_cast<double>(remain) / advance,
                                 kFrameCeiling);
    const double end = static_cast<double>(p.t_in) +
                       std::max(1.0, std::floor(span));
    return static_cast<uint32_t>(std::min(end, kFrameCeiling));
}

// Local length of a sequence: its explicit duration, else the furthest
// block end across video lanes and audio tracks - a music bed longer
// than the picture holds the timeline open.
inline uint32_t sequence_duration(const Document& doc, const Sequence& seq,
                                  int depth = 0) {
    if (seq.duration) return seq.duration;
    if (depth >= kMaxLookDepth) return 0;
    const double eff = effective_fps(doc, seq);
    uint32_t end = 0;
    for (const SeqTrack& t : seq.tracks)
        for (const Placement& p : t.placements)
            end = std::max(
                end, placement_end(p, source_length(doc, p, depth + 1),
                                   placement_ratio(doc, p, eff)));
    for (const AudioTrack& t : seq.audio)
        for (const Placement& p : t.placements)
            end = std::max(
                end, placement_end(p, source_length(doc, p, depth + 1),
                                   placement_ratio(doc, p, eff)));
    return end;
}

// Is this placement playing at a local time? An unbounded placement is
// always live from t_in. Both bounds are whole frames, so testing the
// continuous time and testing its floor agree exactly - the compiler,
// the flatten, the monitor pick and the razor all decide liveness
// through this one predicate (the razor additionally excludes the exact
// head: a cut AT t_in is a no-op, not a zero-width left half).
inline bool placement_active(const Placement& p, uint32_t source_len,
                             double local, double ratio = 1.0) {
    if (local < static_cast<double>(p.t_in)) return false;
    const uint32_t end = placement_end(p, source_len, ratio);
    return end == 0 || local < static_cast<double>(end);
}

// The block a video lane SHOWS at a local time: among the active
// placements the latest-starting wins (an overlap reads as the incoming
// block taking over at its in-point), list order breaking ties. Audio
// never picks - every active placement sums in the mix. The compiler
// and the monitor's click-pick must share this tiebreak or a click
// selects a block the render did not draw.
inline const Placement* placement_winner(
    const Document& doc, const std::vector<Placement>& placements,
    double local, double parent_fps) {
    const Placement* best = nullptr;
    for (const Placement& p : placements) {
        if (!placement_active(p, source_length(doc, p), local,
                              placement_ratio(doc, p, parent_fps)))
            continue;
        if (!best || p.t_in >= best->t_in) best = &p;
    }
    return best;
}

// Local frame -> target frame. The affine map that composes under
// nesting - the hop's clock ratio scales the advance so a pinned
// target's own frames tick at its own rate; callers check
// placement_active first.
inline double placement_source_frame(const Placement& p, double local,
                                     double ratio = 1.0) {
    return (local - static_cast<double>(p.t_in)) *
               static_cast<double>(p.speed) * ratio +
           static_cast<double>(p.source_in);
}

inline constexpr float kDeg2Rad = 0.01745329252f;

// Inverse of the placement's canvas affine: monitor-content UV
// (fractions of the rect) to the block's own frame, which spans
// [-0.5, 0.5] on both axes. Forward is out = a + shift + S*R*(src - a)
// (the anchor is the fixed point of S*R). Mirrors
// layer_blend.comp.slang's forward map; the click picker and the
// overlay drawer must both go through this or the selection box
// detaches from the pixels - in opposite directions.
inline void placement_uv_to_block(const Placement& p, float u, float v,
                                  float aspect, float* bx, float* by) {
    const float rad = p.rotate * kDeg2Rad;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float cxf = u - p.anchor_x - p.pos_x;
    const float cyf = v - p.anchor_y - p.pos_y;
    const float qx = cxf * aspect, qy = cyf;
    const float rx = qx * cs + qy * sn;
    const float ry = -qx * sn + qy * cs;
    const float s = std::max(p.scale, 1e-4f);
    *bx = rx / s / aspect + p.anchor_x - 0.5f;
    *by = ry / s + p.anchor_y - 0.5f;
}

// Head-trim: move the start to `at` with the surviving content held on
// the same source frames. ONE rounding rule (truncate, floored at zero)
// for razor, trim drags and overwrite - paths rounding differently land
// razored and overwritten heads on different source frames at
// fractional speeds.
inline void trim_placement_head(Placement& p, uint32_t at,
                                double ratio = 1.0) {
    const double src =
        placement_source_frame(p, static_cast<double>(at), ratio);
    p.source_in = src <= 0.0 ? 0u : static_cast<uint32_t>(src);
    p.t_in = at;
}

// True when browser bin `from` reaches bin `to` walking parents - the
// no-cycle rule for nesting bins (reparent guard). Depth cap defends
// corrupt files only.
inline bool bin_reaches(const Document& doc, uint64_t from, uint64_t to,
                        int depth = 0) {
    if (!from || depth >= 64) return false;
    if (from == to) return true;
    const Bin* b = doc.find_bin(from);
    return b && bin_reaches(doc, b->parent, to, depth + 1);
}

// True when entity `from` (a look or sequence) already reaches entity
// `to` through nesting - look sources and sequence placements alike: the
// one no-cycle rule, spanning both kinds both ways.
inline bool nest_reaches(const Document& doc, uint64_t from, uint64_t to,
                         int depth = 0) {
    if (from == to) return true;
    if (depth >= kMaxLookDepth) return false;
    if (const Look* l = doc.find_look(from)) {
        for (const Layer& layer : l->layers) {
            if (!layer_is_nested(layer) || !layer.target) continue;
            if (nest_reaches(doc, layer.target, to, depth + 1)) return true;
        }
        return false;
    }
    if (const Sequence* s = doc.find_sequence(from)) {
        for (const SeqTrack& t : s->tracks)
            for (const Placement& p : t.placements) {
                if (!p.target) continue;
                if (nest_reaches(doc, p.target, to, depth + 1)) return true;
            }
        for (const AudioTrack& t : s->audio)
            for (const Placement& p : t.placements) {
                if (!p.target) continue;
                if (nest_reaches(doc, p.target, to, depth + 1)) return true;
            }
    }
    return false;
}

// The layer behind an id. Null when the look holds no such layer.
inline Layer* find_layer(Look& look, uint64_t layer_id) {
    for (Layer& l : look.layers)
        if (l.id == layer_id) return &l;
    return nullptr;
}
inline const Layer* find_layer(const Look& look, uint64_t layer_id) {
    return find_layer(const_cast<Look&>(look), layer_id);
}

// The effect behind an id, with its owning layer. Null when absent.
inline EffectInstance* find_effect(Look& look, uint64_t fx_id,
                                   Layer** owner = nullptr) {
    for (Layer& l : look.layers)
        for (EffectInstance& fx : l.stack)
            if (fx.id == fx_id) {
                if (owner) *owner = &l;
                return &fx;
            }
    return nullptr;
}
inline const EffectInstance* find_effect(const Look& look, uint64_t fx_id,
                                         const Layer** owner = nullptr) {
    return find_effect(const_cast<Look&>(look), fx_id,
                       const_cast<Layer**>(owner));
}

// The group behind an id on one layer. Null when absent.
inline Group* find_group(Layer& layer, uint64_t group_id) {
    for (Group& g : layer.groups)
        if (g.id == group_id) return &g;
    return nullptr;
}
inline const Group* find_group(const Layer& layer, uint64_t group_id) {
    return find_group(const_cast<Layer&>(layer), group_id);
}

// Whether an effect's enclosing group is bypassed (0 = ungrouped).
inline bool group_bypassed(const Layer& layer, uint64_t group_id) {
    if (group_id == 0) return false;
    const Group* g = find_group(layer, group_id);
    return g && g->bypass;
}

// The value node behind an id. Null when the look holds no such node.
inline ValueNode* find_value_node(Look& look, uint64_t node_id) {
    for (ValueNode& n : look.value_nodes)
        if (n.id == node_id) return &n;
    return nullptr;
}
inline const ValueNode* find_value_node(const Look& look, uint64_t node_id) {
    return find_value_node(const_cast<Look&>(look), node_id);
}

// True when value node `from` already reaches `to` through its helper
// inputs - the value graph's no-cycle guard; check before wiring `to`
// into one of `from`'s inputs.
inline bool value_reaches(const Look& look, uint64_t from, uint64_t to,
                          int depth = 0) {
    if (from == to) return true;
    if (depth >= 64) return false;
    const ValueNode* n = find_value_node(look, from);
    if (!n) return false;
    return (n->in_a && value_reaches(look, n->in_a, to, depth + 1)) ||
           (n->in_b && value_reaches(look, n->in_b, to, depth + 1));
}

// The placement behind an id, wherever it lives - a video lane or an
// audio track. Null when the sequence holds no such placement.
inline Placement* find_placement(Sequence& seq, uint64_t placement_id) {
    for (SeqTrack& t : seq.tracks)
        for (Placement& p : t.placements)
            if (p.id == placement_id) return &p;
    for (AudioTrack& t : seq.audio)
        for (Placement& p : t.placements)
            if (p.id == placement_id) return &p;
    return nullptr;
}
inline const Placement* find_placement(const Sequence& seq,
                                       uint64_t placement_id) {
    return find_placement(const_cast<Sequence&>(seq), placement_id);
}

// The chain topology as links: source -> effects in stack order -> Output.
inline std::vector<NodeLink> synthesize_links(const Look& look) {
    std::vector<NodeLink> links;
    for (const Layer& layer : look.layers) {
        uint64_t prev = layer.id;
        for (const EffectInstance& fx : layer.stack) {
            links.push_back({prev, fx.id, 0});
            prev = fx.id;
        }
        links.push_back({prev, 0, 0});
    }
    return links;
}

// Materializes the synthesized links onto a look (first link edit,
// canvas display). Idempotent when links already exist.
inline void ensure_links(Look& look) {
    if (look.links.empty()) look.links = synthesize_links(look);
}

// The EMPTY table means synthesized wiring, so deleting a look's last
// real wire must not fall back into it (the chain would resurrect).
// A dead self-link keeps the table explicit; a real connect prunes it.
inline bool link_is_tombstone(const NodeLink& l) {
    return l.from == 0 && l.to == 0 && l.to_port == 9999;
}
inline void seal_links(Look& look) {
    if (look.links.empty()) look.links.push_back({0, 0, 9999});
}
inline bool prune_tombstone(Look& look) {
    for (auto it = look.links.begin(); it != look.links.end(); ++it)
        if (link_is_tombstone(*it)) {
            look.links.erase(it);
            return true;
        }
    return false;
}

}  // namespace looks::doc
