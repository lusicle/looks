#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doc/effect_instance.h"
#include "doc/modulation.h"

namespace looks::doc {

struct Asset {
    uint64_t id = 0;
    std::string name;
    std::string path;      // mezzanine bundle path
    uint32_t frame_count = 0;
    double fps = 0.0;
    uint32_t width = 0, height = 0;
    // Still-image length in frames. 0 = as imported.
    uint32_t still_duration_frames = 0;
    // A true still counts frames on the project clock: no conform rate.
    bool still = false;
    // Browser bin. 0 = the project root.
    uint64_t bin = 0;
};

// Bins nest by parent id. 0 = the root.
// Bins hold no members: looks, sequences and assets carry the bin id.
struct Bin {
    uint64_t id = 0;
    std::string name;
    uint64_t parent = 0;
};

// Local frame to target frame is affine: (local - t_in) * speed + source_in.
struct Placement {
    uint64_t id = 0;
    // The look or sequence this block plays. 0 = unbound.
    uint64_t target = 0;
    uint32_t t_in = 0;
    uint32_t t_out = 0;      // exclusive local end; 0 = run to the target end
    uint32_t source_in = 0;  // target-local frame shown at t_in
    float speed = 1.0f;
    // Link group id: every timing edit applies group-wide. 0 = unlinked.
    uint64_t link = 0;
    float audio_gain = 1.0f;   // 0..2, linear
    bool audio_mute = false;
    // Motion, video lanes: canvas fractions, +x right, +y down, 0 = centered.
    // rotate is in degrees about the anchor.
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float scale = 1.0f;
    float rotate = 0.0f;
    float opacity = 1.0f;
    // Scale and rotate pivot in block-local fractions. 0.5 = the centre.
    float anchor_x = 0.5f;
    float anchor_y = 0.5f;
};

inline bool placement_has_transform(const Placement& p) {
    return p.pos_x != 0.0f || p.pos_y != 0.0f || p.scale != 1.0f ||
           p.rotate != 0.0f;
}

// hidden drops the lane from render and export, not only from the view.
struct SeqTrack {
    uint64_t id = 0;
    std::string name;
    std::vector<Placement> placements;
    bool hidden = false;
    bool lock = false;
};

// Audio sums, so audio track order is display only.
struct AudioTrack {
    uint64_t id = 0;
    std::string name;
    std::vector<Placement> placements;
    float gain = 1.0f;   // 0..2, linear
    bool mute = false;
    bool lock = false;   // edit guard only; the mix ignores it
};

enum class LayerSourceKind : uint32_t {
    Media = 0,
    // Generator values match gen.comp.slang: renumbering needs a shader edit.
    Solid,
    Gradient,
    Noise,
    TestPattern,
    Oscillator,
    // Shape reuses gen_scale as size and gen_angle as feather.
    Shape,
    LookRef,
    SequenceRef,
    Count,
};

// to 0 = the look Output. to_port 0 = In, 1 = matte, 2+ = aux inputs.
// On the Output port 1 is the audio-in; a group id takes port 1 only.
struct NodeLink {
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t to_port = 0;
};

struct CanvasFrame {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f;
    float w = 480.0f, h = 360.0f;
    std::string title;
    // Colour tag: 0 = none, 1..8 = palette hue.
    uint32_t color = 0;
};

// Members carry group_id. A group never changes the render order.
struct Group {
    uint64_t id = 0;
    std::string name;
    bool folded = false;
    bool bypass = false;
    // Exposed params are direct aliases: same value, no hidden offset.
    std::vector<ParamKey> exposed;
    // Mod targets: ParamKey{id | kGroupParamBit, kWetParam or kOpacityParam}.
    float wet = 1.0f;
    float opacity = 1.0f;
    // Slot ids are one-port passthroughs in the link table, in order.
    // Ports are index-derived and links are id-keyed, so wires follow.
    std::vector<uint64_t> inputs;
    // Which member feeds the card Out. 0 = the last member.
    uint64_t face_out = 0;
    // Node-canvas position of the folded card. (0,0) = unplaced.
    float node_x = 0.0f;
    float node_y = 0.0f;
    // Scoped-view positions of the In/Out nodes. (0,0) = unplaced.
    float in_x = 0.0f, in_y = 0.0f;
    float out_x = 0.0f, out_y = 0.0f;
};

// Canvas fractions. The handles are offsets from the anchor; 0 = a corner.
struct PathPoint {
    float ax = 0.0f, ay = 0.0f;
    float in_dx = 0.0f, in_dy = 0.0f;
    float out_dx = 0.0f, out_dy = 0.0f;
};

struct Layer {
    uint64_t id = 0;
    std::string name;
    LayerSourceKind source = LayerSourceKind::Media;
    // slip is a static media in-point: local frame 0 reads media frame slip.
    uint64_t asset = 0;
    uint32_t slip = 0;
    // Timeline lock (media only): the node reads the ROOT clock.
    // Placement speed and source_in stop applying to a locked node.
    bool timeline_lock = false;
    uint64_t target = 0;
    // Generators: color_a start, color_b end, gen_scale cells, gen_angle dir.
    float color_a[3] = {0.5f, 0.5f, 0.5f};
    float color_b[3] = {0.1f, 0.1f, 0.1f};
    float gen_scale = 6.0f;
    float gen_angle = 0.0f;
    // Oscillator phase in percent of one period. Values can pass 100.
    float gen_phase = 0.0f;
    // Oscillator waveform: 0 sine bars, 1 rings, 2 plasma.
    uint32_t osc_shape = 0;
    // Custom shape path: closed = a filled matte, open = a feathered stroke.
    std::vector<PathPoint> path;
    bool path_closed = true;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
    // The transform applies to the source before the stack, premultiplied.
    float crop_l = 0.0f, crop_r = 0.0f;   // fraction of frame, 0..0.45
    float crop_t = 0.0f, crop_b = 0.0f;
    bool flip_h = false, flip_v = false;
    float xf_scale = 1.0f;                // about the anchor, 0.25..4
    float xf_rotate = 0.0f;               // degrees, -180..180
    // Pivot in frame fractions. 0.5 = the frame centre.
    float xf_anchor_x = 0.5f;
    float xf_anchor_y = 0.5f;
    // Node-canvas position of the source node. (0,0) = unplaced.
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

// kMaxLayers bounds graph nodes, not edit length.
inline constexpr size_t kMaxLayers = 16;
inline constexpr size_t kMaxPlacementsPerTrack = 256;
inline constexpr size_t kMaxLooks = 256;
inline constexpr int kMaxLookDepth = 8;   // nesting guard, both entities
inline constexpr float kMaxSpeed = 4.0f;  // time-remap speed range 0..4

// All-zero = unset = inherit the project format.
// fps pins the entity local clock; w and h pin its render canvas.
struct EntityFormat {
    uint32_t w = 0, h = 0;
    double fps = 0.0;
};

inline bool format_has_fps(const EntityFormat& f) { return f.fps > 0.0; }
inline bool format_has_canvas(const EntityFormat& f) {
    return f.w > 0 && f.h > 0;
}

struct Look {
    uint64_t id = 0;
    std::string name;
    // Local length in frames. 0 = derived from the longest source.
    uint32_t duration = 0;
    EntityFormat format;
    // Browser bin. 0 = the project root.
    uint64_t bin = 0;
    // false = the voice is the port-0 In fan-in; true = a port-1 audio-in.
    // A split Output with nothing wired is silent, never a fallback.
    bool audio_split = false;

    std::vector<Layer> layers;
    std::vector<NodeLink> links;
    std::vector<CanvasFrame> frames;

    std::vector<ValueNode> value_nodes;
    std::vector<ModRoute> mod_routes;
    // Keyframe lanes key on this look's own clock.
    std::vector<KeyframeLane> lanes;
    Snapshot snapshots[3];
    // morph_pos is a mod target: ParamKey{0, 0}.
    int morph_from = 0;
    int morph_to = 1;
    float morph_pos = 0.0f;

    // Node-canvas position of the Output node. (0,0) = unplaced.
    float out_node_x = 0.0f;
    float out_node_y = 0.0f;
};

// tracks[0] is the bottom lane. The last lane composites on top.
struct Sequence {
    uint64_t id = 0;
    std::string name;
    // Local length in frames. 0 = derived from the furthest block end.
    uint32_t duration = 0;
    EntityFormat format;
    // Browser bin. 0 = the project root.
    uint64_t bin = 0;

    std::vector<SeqTrack> tracks;
    std::vector<AudioTrack> audio;

    // Trim [in, out) plays and exports: out 0 = the end. loop 0/0 = off.
    uint32_t trim_in = 0;
    uint32_t trim_out = 0;
    uint32_t loop_in = 0;
    uint32_t loop_out = 0;
    std::vector<uint32_t> markers;
};

struct Document {
    std::string name = "untitled";
    std::vector<Asset> assets;
    std::vector<Look> looks;
    std::vector<Sequence> sequences;
    uint64_t root_sequence = 0;
    std::vector<Bin> bins;

    uint64_t master_seed = 0;
    // Project frame rate. 0 = derive from the first asset.
    double fps = 0.0;
    // Project canvas. 0/0 = derive from the first bound asset.
    uint32_t canvas_w = 0, canvas_h = 0;
    // Frame render cache budget in MiB. 0 disables the cache.
    uint32_t cache_mb = 2048;
    // Preview only: export always renders full resolution.
    bool use_proxy = false;
    // The command stack bumps this on every execute, undo and redo.
    uint64_t revision = 0;

    // One counter: ids stay unique document-wide across all kinds.
    uint64_t next_effect_id = 1;
    uint64_t next_route_id = 1;

    float speed = 1.0f;
    uint32_t time_mode = 0;   // 0 forward, 1 reverse, 2 ping-pong

    std::string sidechain_path;
    bool sidechain_mux = false;
    // Shifts audio against video everywhere: curves, monitoring, export.
    float audio_offset_ms = 0.0f;

    float export_bitrate_mbps = 8.0f;
    // 1 = source size, 2 and 4 = half and quarter.
    uint32_t export_scale = 1;
    bool export_audio = true;

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
    // For live ids only: a stale id falls back to the first entity.
    // A project may hold no looks and no sequences. These fall back to a
    // shared empty entity, so a stale id reads empty and never aborts.
    static Look& empty_look() {
        static Look none;
        return none;
    }
    static Sequence& empty_sequence() {
        static Sequence none;
        return none;
    }
    Look& look(uint64_t id) {
        Look* l = find_look(id);
        return l ? *l : empty_look();
    }
    const Look& look(uint64_t id) const {
        const Look* l = find_look(id);
        return l ? *l : empty_look();
    }
    Sequence& sequence(uint64_t id) {
        Sequence* s = find_sequence(id);
        return s ? *s : empty_sequence();
    }
    const Sequence& sequence(uint64_t id) const {
        const Sequence* s = find_sequence(id);
        return s ? *s : empty_sequence();
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
    const Asset* primary_asset() const {
        return assets.empty() ? nullptr : &assets.front();
    }
};

// Returns even dimensions: the NV12 and codec paths need them.
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

inline double project_fps(const Document& doc) {
    if (doc.fps > 0.0) return doc.fps;
    for (const Asset& a : doc.assets)
        if (a.fps > 0.0) return a.fps;
    return 30.0;
}

// A nesting hop ratio is child fps / parent fps: child frames per parent.
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

// Media frames per clock frame. True stills and unknown rates play 1:1.
inline double media_conform_rate(const Document& doc, const Asset& a,
                                 double clock_fps) {
    (void)doc;
    if (a.still) return 1.0;
    return (a.fps > 0.0 && clock_fps > 0.0) ? a.fps / clock_fps : 1.0;
}

// Local frames to clock frames. ceil keeps the last partial frame.
inline uint32_t conform_frames(uint32_t frames, double ratio) {
    if (frames == 0 || ratio <= 0.0) return frames;
    return static_cast<uint32_t>(
        std::ceil(static_cast<double>(frames) / ratio));
}

// Returns frames on the owning look clock. 0 = unbounded.
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

// Returns frames on the look's own clock. 0 = unbounded.
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

// Returns frames on the target entity's own clock. 0 = unbound.
inline uint32_t source_length(const Document& doc, const Placement& p,
                              int depth = 0) {
    if (!p.target || depth >= kMaxLookDepth) return 0;
    if (const Look* l = doc.find_look(p.target))
        return look_duration(doc, *l, depth);
    if (const Sequence* s = doc.find_sequence(p.target))
        return sequence_duration(doc, *s, depth);
    return 0;
}

// Target frames per parent frame. The child advance is speed * ratio.
inline double placement_ratio(const Document& doc, const Placement& p,
                              double parent_fps) {
    if (!p.target || parent_fps <= 0.0) return 1.0;
    return entity_fps(doc, p.target) / parent_fps;
}

// source_len and source_in are target frames. 0 = unbounded.
inline uint32_t placement_end(const Placement& p, uint32_t source_len,
                              double ratio = 1.0) {
    if (p.t_out) return p.t_out;
    if (!source_len) return 0;
    const uint32_t remain =
        source_len > p.source_in ? source_len - p.source_in : 0;
    if (!remain) return p.t_in + 1;   // degenerate: keep one frame
    // Speed 0 runs forever: the ceiling keeps the result inside uint32.
    constexpr double kFrameCeiling = 1.0e9;
    const double advance = std::max(
        static_cast<double>(p.speed) * (ratio > 0.0 ? ratio : 1.0), 1e-6);
    const double span = std::min(static_cast<double>(remain) / advance,
                                 kFrameCeiling);
    const double end = static_cast<double>(p.t_in) +
                       std::max(1.0, std::floor(span));
    return static_cast<uint32_t>(std::min(end, kFrameCeiling));
}

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

// The one liveness rule: the continuous test and its floor agree exactly.
inline bool placement_active(const Placement& p, uint32_t source_len,
                             double local, double ratio = 1.0) {
    if (local < static_cast<double>(p.t_in)) return false;
    const uint32_t end = placement_end(p, source_len, ratio);
    return end == 0 || local < static_cast<double>(end);
}

// Latest start wins, list order breaks ties. Render and pick share it.
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

// Callers must check placement_active first.
inline double placement_source_frame(const Placement& p, double local,
                                     double ratio = 1.0) {
    return (local - static_cast<double>(p.t_in)) *
               static_cast<double>(p.speed) * ratio +
           static_cast<double>(p.source_in);
}

inline constexpr float kDeg2Rad = 0.01745329252f;

// UV fractions in, block frame out: the block spans -0.5..0.5 on each axis.
// Keep this the exact inverse of the layer_blend.comp.slang forward map.
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

// One rounding rule for razor, trim and overwrite: truncate at zero.
inline void trim_placement_head(Placement& p, uint32_t at,
                                double ratio = 1.0) {
    const double src =
        placement_source_frame(p, static_cast<double>(at), ratio);
    p.source_in = src <= 0.0 ? 0u : static_cast<uint32_t>(src);
    p.t_in = at;
}

// No-cycle guard for bin reparent. The depth cap defends bad files only.
inline bool bin_reaches(const Document& doc, uint64_t from, uint64_t to,
                        int depth = 0) {
    if (!from || depth >= 64) return false;
    if (from == to) return true;
    const Bin* b = doc.find_bin(from);
    return b && bin_reaches(doc, b->parent, to, depth + 1);
}

// The one no-cycle guard for nesting, looks and sequences both ways.
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

inline Layer* find_layer(Look& look, uint64_t layer_id) {
    for (Layer& l : look.layers)
        if (l.id == layer_id) return &l;
    return nullptr;
}
inline const Layer* find_layer(const Look& look, uint64_t layer_id) {
    return find_layer(const_cast<Look&>(look), layer_id);
}

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

inline Group* find_group(Layer& layer, uint64_t group_id) {
    for (Group& g : layer.groups)
        if (g.id == group_id) return &g;
    return nullptr;
}
inline const Group* find_group(const Layer& layer, uint64_t group_id) {
    return find_group(const_cast<Layer&>(layer), group_id);
}

inline bool group_bypassed(const Layer& layer, uint64_t group_id) {
    if (group_id == 0) return false;
    const Group* g = find_group(layer, group_id);
    return g && g->bypass;
}

inline Group* find_group(Look& look, uint64_t group_id,
                         size_t* layer_index = nullptr) {
    for (size_t li = 0; li < look.layers.size(); ++li)
        if (Group* g = find_group(look.layers[li], group_id)) {
            if (layer_index) *layer_index = li;
            return g;
        }
    return nullptr;
}
inline const Group* find_group(const Look& look, uint64_t group_id,
                               size_t* layer_index = nullptr) {
    return find_group(const_cast<Look&>(look), group_id, layer_index);
}

// The one definition of a group card's Out. 0 = an empty group.
inline uint64_t group_face_member(const Layer& layer, const Group& g) {
    uint64_t last = 0, bind = 0;
    for (const EffectInstance& e : layer.stack)
        if (e.group_id == g.id) {
            last = e.id;
            if (e.id == g.face_out) bind = e.id;
        }
    return bind ? bind : last;
}
inline uint64_t group_face_member(const Look& look, uint64_t group_id) {
    for (const Layer& l : look.layers)
        if (const Group* g = find_group(l, group_id))
            return group_face_member(l, *g);
    return 0;
}

inline Group* group_of_input(Look& look, uint64_t slot_id,
                             size_t* layer_index = nullptr) {
    if (!slot_id) return nullptr;
    for (size_t li = 0; li < look.layers.size(); ++li)
        for (Group& g : look.layers[li].groups)
            for (uint64_t s : g.inputs)
                if (s == slot_id) {
                    if (layer_index) *layer_index = li;
                    return &g;
                }
    return nullptr;
}
inline const Group* group_of_input(const Look& look, uint64_t slot_id,
                                   size_t* layer_index = nullptr) {
    return group_of_input(const_cast<Look&>(look), slot_id, layer_index);
}

// Deletes leave dangling links, so every wire walk must skip dead feeds.
inline bool wire_producer_live(const Look& look, uint64_t id) {
    return id && (find_layer(look, id) || find_effect(look, id) ||
                  group_of_input(look, id));
}

// Link-vector order is stacking order: a slot reads its bottom feed.
inline uint64_t hop_group_inputs(const Look& look,
                                 const std::vector<NodeLink>& links,
                                 uint64_t id) {
    for (int guard = 0; guard < 16 && id; ++guard) {
        if (!group_of_input(look, id)) return id;
        uint64_t next = 0;
        for (const NodeLink& l : links)
            if (l.to == id && l.to_port == 0 &&
                wire_producer_live(look, l.from)) {
                next = l.from;
                break;
            }
        id = next;
    }
    return id;
}

inline ValueNode* find_value_node(Look& look, uint64_t node_id) {
    for (ValueNode& n : look.value_nodes)
        if (n.id == node_id) return &n;
    return nullptr;
}
inline const ValueNode* find_value_node(const Look& look, uint64_t node_id) {
    return find_value_node(const_cast<Look&>(look), node_id);
}

inline CanvasFrame* find_frame(Look& look, uint64_t frame_id) {
    for (CanvasFrame& f : look.frames)
        if (f.id == frame_id) return &f;
    return nullptr;
}

// Value-graph no-cycle guard. Call it before you wire an input.
inline bool value_reaches(const Look& look, uint64_t from, uint64_t to,
                          int depth = 0) {
    if (from == to) return true;
    if (depth >= 64) return false;
    const ValueNode* n = find_value_node(look, from);
    if (!n) return false;
    return (n->in_a && value_reaches(look, n->in_a, to, depth + 1)) ||
           (n->in_b && value_reaches(look, n->in_b, to, depth + 1));
}

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

struct PlacementSlot {
    std::vector<Placement>* list = nullptr;
    uint64_t track_id = 0;
    bool audio = false;
    size_t index = 0;
};
inline bool find_placement_slot(Sequence& seq, uint64_t placement_id,
                                PlacementSlot* out) {
    for (SeqTrack& t : seq.tracks)
        for (size_t i = 0; i < t.placements.size(); ++i)
            if (t.placements[i].id == placement_id) {
                *out = {&t.placements, t.id, false, i};
                return true;
            }
    for (AudioTrack& t : seq.audio)
        for (size_t i = 0; i < t.placements.size(); ++i)
            if (t.placements[i].id == placement_id) {
                *out = {&t.placements, t.id, true, i};
                return true;
            }
    return false;
}

// Do not read look.links directly for wiring: use effective_links.
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

// The result can point into scratch, so scratch must outlive it.
inline const std::vector<NodeLink>& effective_links(
    const Look& look, std::vector<NodeLink>& scratch) {
    if (!look.links.empty()) return look.links;
    scratch = synthesize_links(look);
    return scratch;
}

inline void ensure_links(Look& look) {
    if (look.links.empty()) look.links = synthesize_links(look);
}

// An empty table means synthesized links: a tombstone keeps it explicit.
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
