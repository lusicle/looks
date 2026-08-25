#include "doc/serialize.h"

#include <algorithm>

#include "doc/effects.h"
#include "util/file.h"

namespace looks::doc {

namespace {

using json::Array;
using json::Value;

// ---- enum <-> string tables. Names are the stable on-disk vocabulary;
// enum order can change without breaking saved files.

const char* const kBlendNames[] = {"normal", "add", "multiply", "screen",
                                   "difference"};
const char* const kSourceKindNames[] = {"media", "solid", "gradient",
                                        "noise", "test",  "oscillator",
                                        "shape", "look",  "sequence"};
static_assert(sizeof(kSourceKindNames) / sizeof(kSourceKindNames[0]) ==
                  static_cast<size_t>(LayerSourceKind::Count),
              "source kind names track the enum");
const char* const kModSourceNames[] = {
    "lfo",         "drift",       "audio_low",    "audio_mid",
    "audio_high",  "audio_onset", "video_motion", "video_brightness",
    "lfo_beat",    "envelope",    "video_cut",    "beat",
    "video_sample", "video_region", "math",       "normalise",
    "camera"};
static_assert(sizeof(kModSourceNames) / sizeof(kModSourceNames[0]) ==
                  static_cast<size_t>(ModSourceType::Count),
              "mod source names track the enum");
const char* const kValueOpNames[] = {"add", "subtract", "multiply",
                                     "divide", "min", "max", "floor",
                                     "absolute"};
static_assert(sizeof(kValueOpNames) / sizeof(kValueOpNames[0]) ==
                  static_cast<size_t>(ValueOp::Count),
              "value op names track the enum");
const char* const kLfoShapeNames[] = {"sine", "triangle", "square",
                                      "sample_hold"};
const char* const kCurveNames[] = {"linear", "exp", "scurve", "inverted"};

template <size_t N>
const char* enum_name(const char* const (&names)[N], uint32_t index) {
    return index < N ? names[index] : names[0];
}

template <size_t N>
uint32_t enum_index(const char* const (&names)[N], const std::string& s,
                    uint32_t fallback = 0) {
    for (uint32_t i = 0; i < N; ++i)
        if (s == names[i]) return i;
    return fallback;
}

std::optional<EffectType> effect_type_from_id(const std::string& id) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(EffectType::Count); ++i)
        if (id == effect_info(static_cast<EffectType>(i)).id)
            return static_cast<EffectType>(i);
    return std::nullopt;
}

Value f3_to_json(const float (&v)[3]) {
    Array a;
    for (float f : v) a.push_back(Value(static_cast<double>(f)));
    return Value(std::move(a));
}

void f3_from_json(const Value& v, float (&out)[3]) {
    const Array& a = v.array();
    for (size_t i = 0; i < 3 && i < a.size(); ++i)
        out[i] = static_cast<float>(a[i].as_number(out[i]));
}

float num(const Value& obj, std::string_view key, float fallback) {
    return static_cast<float>(obj.get(key).as_number(fallback));
}

// ---- modulation pieces

Value param_key_to_json(const ParamKey& k) {
    Value v = Value::make_object();
    // Layer keys carry bit 62, past the JSON number's 2^53 exact-integer
    // range — store the bare id in its own field instead.
    if (k.effect_id & kLayerParamBit)
        v.set("layer", static_cast<int64_t>(k.effect_id & ~kLayerParamBit));
    else
        v.set("effect", static_cast<int64_t>(k.effect_id));
    v.set("param", k.param_index);
    return v;
}

ParamKey param_key_from_json(const Value& v) {
    ParamKey k;
    const int64_t layer_id = v.get("layer").as_int(-1);
    if (layer_id >= 0)
        k.effect_id = static_cast<uint64_t>(layer_id) | kLayerParamBit;
    else
        k.effect_id = static_cast<uint64_t>(v.get("effect").as_int(0));
    k.param_index = static_cast<int>(v.get("param").as_int(0));
    return k;
}

Value mod_source_to_json(const ModSource& s) {
    Value v = Value::make_object();
    v.set("type", enum_name(kModSourceNames, static_cast<uint32_t>(s.type)));
    v.set("shape", enum_name(kLfoShapeNames, static_cast<uint32_t>(s.shape)));
    v.set("rate_hz", static_cast<double>(s.rate_hz));
    v.set("phase", static_cast<double>(s.phase));
    v.set("seed", static_cast<int64_t>(s.seed));
    v.set("attack", static_cast<double>(s.attack));
    v.set("decay", static_cast<double>(s.decay));
    v.set("trigger", static_cast<int64_t>(s.trigger));
    // Sampling geometry only for the video-sampling types — older files
    // stay byte-identical through a load/save roundtrip. The camera
    // node's channel selector rides the same field.
    if (s.type == ModSourceType::VideoSample ||
        s.type == ModSourceType::VideoRegion) {
        v.set("px", static_cast<double>(s.px));
        v.set("py", static_cast<double>(s.py));
        v.set("pw", static_cast<double>(s.pw));
        v.set("ph", static_cast<double>(s.ph));
        v.set("channel", static_cast<int64_t>(s.channel));
    } else if (s.type == ModSourceType::Camera) {
        v.set("channel", static_cast<int64_t>(s.channel));
        // The plane region rides the sampling-geometry fields.
        v.set("px", static_cast<double>(s.px));
        v.set("py", static_cast<double>(s.py));
        v.set("pw", static_cast<double>(s.pw));
        v.set("ph", static_cast<double>(s.ph));
        if (s.anchor) v.set("anchor", static_cast<int64_t>(s.anchor));
    }
    return v;
}

ModSource mod_source_from_json(const Value& v) {
    ModSource s;
    s.type = static_cast<ModSourceType>(
        enum_index(kModSourceNames, v.get("type").as_string()));
    s.shape = static_cast<LfoShape>(
        enum_index(kLfoShapeNames, v.get("shape").as_string()));
    s.rate_hz = num(v, "rate_hz", 1.0f);
    s.phase = num(v, "phase", 0.0f);
    s.seed = static_cast<uint64_t>(v.get("seed").as_int(0));
    s.attack = num(v, "attack", 0.02f);
    s.decay = num(v, "decay", 0.4f);
    s.trigger = static_cast<uint32_t>(v.get("trigger").as_int(0));
    s.px = num(v, "px", 0.5f);
    s.py = num(v, "py", 0.5f);
    s.pw = num(v, "pw", 0.25f);
    s.ph = num(v, "ph", 0.25f);
    s.channel = static_cast<uint32_t>(v.get("channel").as_int(0));
    s.anchor = static_cast<uint32_t>(v.get("anchor").as_int(0));
    return s;
}

Value value_node_to_json(const ValueNode& n) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(n.id));
    v.set("source", mod_source_to_json(n.source));
    if (n.source.type == ModSourceType::Math) {
        v.set("op", enum_name(kValueOpNames, static_cast<uint32_t>(n.op)));
        v.set("const_a", static_cast<double>(n.const_a));
        v.set("const_b", static_cast<double>(n.const_b));
    }
    if (n.source.type == ModSourceType::Normalise) {
        v.set("const_a", static_cast<double>(n.const_a));
        v.set("const_b", static_cast<double>(n.const_b));   // window mult
        v.set("in_min", static_cast<double>(n.in_min));
        v.set("in_max", static_cast<double>(n.in_max));
    }
    if (n.in_a) v.set("in_a", static_cast<int64_t>(n.in_a));
    if (n.in_b) v.set("in_b", static_cast<int64_t>(n.in_b));
    if (n.audio_src) v.set("audio_src", static_cast<int64_t>(n.audio_src));
    if (n.node_x != 0.0f || n.node_y != 0.0f) {
        v.set("node_x", static_cast<double>(n.node_x));
        v.set("node_y", static_cast<double>(n.node_y));
    }
    return v;
}

ValueNode value_node_from_json(const Value& v) {
    ValueNode n;
    n.id = static_cast<uint64_t>(v.get("id").as_int(0));
    n.source = mod_source_from_json(v.get("source"));
    n.op = static_cast<ValueOp>(
        enum_index(kValueOpNames, v.get("op").as_string()));
    n.in_a = static_cast<uint64_t>(v.get("in_a").as_int(0));
    n.in_b = static_cast<uint64_t>(v.get("in_b").as_int(0));
    n.audio_src = static_cast<uint64_t>(v.get("audio_src").as_int(0));
    n.const_a = num(v, "const_a", 0.0f);
    n.const_b = num(v, "const_b", 1.0f);
    n.in_min = num(v, "in_min", 0.0f);
    n.in_max = num(v, "in_max", 1.0f);
    n.node_x = num(v, "node_x", 0.0f);
    n.node_y = num(v, "node_y", 0.0f);
    return n;
}

Value route_to_json(const ModRoute& r) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(r.id));
    v.set("node", static_cast<int64_t>(r.node));
    v.set("target", param_key_to_json(r.target));
    v.set("curve", enum_name(kCurveNames, static_cast<uint32_t>(r.curve)));
    return v;
}

ModRoute route_from_json(const Value& v) {
    ModRoute r;
    r.id = static_cast<uint64_t>(v.get("id").as_int(0));
    r.node = static_cast<uint64_t>(v.get("node").as_int(0));
    r.target = param_key_from_json(v.get("target"));
    r.curve = static_cast<ResponseCurve>(
        enum_index(kCurveNames, v.get("curve").as_string()));
    return r;
}

Value lane_to_json(const KeyframeLane& lane) {
    Value v = Value::make_object();
    v.set("target", param_key_to_json(lane.target));
    if (lane.loop) v.set("loop", true);
    if (lane.muted) v.set("muted", true);
    Value keys = Value::make_array();
    for (const Keyframe& k : lane.keys) {
        Value kv = Value::make_object();
        kv.set("frame", k.frame);
        kv.set("value", static_cast<double>(k.value));
        kv.set("out_dx", static_cast<double>(k.out_dx));
        kv.set("out_dy", static_cast<double>(k.out_dy));
        kv.set("in_dx", static_cast<double>(k.in_dx));
        kv.set("in_dy", static_cast<double>(k.in_dy));
        kv.set("hold", k.hold);
        keys.push(std::move(kv));
    }
    v.set("keys", std::move(keys));
    return v;
}

KeyframeLane lane_from_json(const Value& v) {
    KeyframeLane lane;
    lane.target = param_key_from_json(v.get("target"));
    lane.loop = v.get("loop").as_bool(false);
    lane.muted = v.get("muted").as_bool(false);
    for (const Value& kv : v.get("keys").array()) {
        Keyframe k;
        k.frame = kv.get("frame").as_number(0.0);
        k.value = num(kv, "value", 0.0f);
        k.out_dx = num(kv, "out_dx", 0.0f);
        k.out_dy = num(kv, "out_dy", 0.0f);
        k.in_dx = num(kv, "in_dx", 0.0f);
        k.in_dy = num(kv, "in_dy", 0.0f);
        k.hold = kv.get("hold").as_bool(false);
        lane.keys.push_back(k);
    }
    // eval_lane assumes ascending frames; the command path sorts on every
    // edit but hand-authored files may not — sort here so both hold.
    std::stable_sort(lane.keys.begin(), lane.keys.end(),
                     [](const Keyframe& a, const Keyframe& b) {
                         return a.frame < b.frame;
                     });
    return lane;
}

Value snapshot_to_json(const Snapshot& s) {
    Value v = Value::make_object();
    v.set("valid", s.valid);
    Value entries = Value::make_array();
    for (const SnapshotEntry& e : s.entries) {
        Value ev = Value::make_object();
        ev.set("effect", static_cast<int64_t>(e.effect_id));
        Array params;
        for (float p : e.params) params.push_back(Value(static_cast<double>(p)));
        ev.set("params", Value(std::move(params)));
        ev.set("wet", static_cast<double>(e.wet));
        ev.set("opacity", static_cast<double>(e.opacity));
        entries.push(std::move(ev));
    }
    v.set("entries", std::move(entries));
    return v;
}

Snapshot snapshot_from_json(const Value& v) {
    Snapshot s;
    s.valid = v.get("valid").as_bool(false);
    for (const Value& ev : v.get("entries").array()) {
        SnapshotEntry e;
        e.effect_id = static_cast<uint64_t>(ev.get("effect").as_int(0));
        for (const Value& p : ev.get("params").array())
            e.params.push_back(static_cast<float>(p.as_number(0.0)));
        e.wet = num(ev, "wet", 1.0f);
        e.opacity = num(ev, "opacity", 1.0f);
        s.entries.push_back(std::move(e));
    }
    return s;
}

// ---- placements (shared by video lanes and audio tracks)

Value placement_to_json(const Placement& p) {
    Value pl = Value::make_object();
    pl.set("id", static_cast<int64_t>(p.id));
    if (p.target) pl.set("target", static_cast<int64_t>(p.target));
    pl.set("t_in", static_cast<int64_t>(p.t_in));
    pl.set("t_out", static_cast<int64_t>(p.t_out));
    pl.set("source_in", static_cast<int64_t>(p.source_in));
    pl.set("speed", static_cast<double>(p.speed));
    if (p.link) pl.set("link", static_cast<int64_t>(p.link));
    if (p.audio_gain != 1.0f)
        pl.set("audio_gain", static_cast<double>(p.audio_gain));
    if (p.audio_mute) pl.set("audio_mute", true);
    if (p.pos_x != 0.0f) pl.set("x", static_cast<double>(p.pos_x));
    if (p.pos_y != 0.0f) pl.set("y", static_cast<double>(p.pos_y));
    if (p.scale != 1.0f) pl.set("scale", static_cast<double>(p.scale));
    if (p.rotate != 0.0f) pl.set("rotate", static_cast<double>(p.rotate));
    if (p.opacity != 1.0f)
        pl.set("opacity", static_cast<double>(p.opacity));
    return pl;
}

Placement placement_from_json(const Value& pl) {
    Placement p;
    p.id = static_cast<uint64_t>(pl.get("id").as_int(0));
    p.target = static_cast<uint64_t>(pl.get("target").as_int(0));
    p.t_in = static_cast<uint32_t>(pl.get("t_in").as_int(0));
    p.t_out = static_cast<uint32_t>(pl.get("t_out").as_int(0));
    p.source_in = static_cast<uint32_t>(pl.get("source_in").as_int(0));
    p.speed = num(pl, "speed", 1.0f);
    p.link = static_cast<uint64_t>(pl.get("link").as_int(0));
    p.audio_gain = num(pl, "audio_gain", 1.0f);
    p.audio_mute = pl.get("audio_mute").as_bool(false);
    p.pos_x = num(pl, "x", 0.0f);
    p.pos_y = num(pl, "y", 0.0f);
    p.scale = num(pl, "scale", 1.0f);
    p.rotate = num(pl, "rotate", 0.0f);
    p.opacity = num(pl, "opacity", 1.0f);
    return p;
}

// ---- layers

Value layer_to_json(const Layer& l) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(l.id));
    v.set("name", l.name);
    v.set("source",
          enum_name(kSourceKindNames, static_cast<uint32_t>(l.source)));
    if (l.asset) v.set("asset", static_cast<int64_t>(l.asset));
    if (l.slip) v.set("slip", static_cast<int64_t>(l.slip));
    if (l.timeline_lock) v.set("timeline_lock", true);
    if (l.target) v.set("target", static_cast<int64_t>(l.target));
    v.set("color_a", f3_to_json(l.color_a));
    v.set("color_b", f3_to_json(l.color_b));
    v.set("gen_scale", static_cast<double>(l.gen_scale));
    v.set("gen_angle", static_cast<double>(l.gen_angle));
    if (l.gen_phase != 0.0f)
        v.set("gen_phase", static_cast<double>(l.gen_phase));
    if (l.osc_shape)
        v.set("osc_shape", static_cast<int64_t>(l.osc_shape));
    if (!l.path.empty()) {
        // Custom shape path: [ax,ay,in_dx,in_dy,out_dx,out_dy] per point.
        Value pts = Value::make_array();
        for (const PathPoint& p : l.path) {
            Value pt = Value::make_array();
            const float f[6] = {p.ax, p.ay, p.in_dx, p.in_dy,
                                p.out_dx, p.out_dy};
            for (float c : f) pt.push(static_cast<double>(c));
            pts.push(std::move(pt));
        }
        v.set("path", std::move(pts));
        if (!l.path_closed) v.set("path_open", true);
    }
    v.set("blend", enum_name(kBlendNames, static_cast<uint32_t>(l.blend)));
    v.set("opacity", static_cast<double>(l.opacity));
    v.set("visible", l.visible);
    // Transform: written only when non-default so
    // untransformed projects stay byte-stable.
    if (layer_has_transform(l)) {
        Value xf = Value::make_object();
        xf.set("crop_l", static_cast<double>(l.crop_l));
        xf.set("crop_r", static_cast<double>(l.crop_r));
        xf.set("crop_t", static_cast<double>(l.crop_t));
        xf.set("crop_b", static_cast<double>(l.crop_b));
        xf.set("flip_h", l.flip_h);
        xf.set("flip_v", l.flip_v);
        xf.set("scale", static_cast<double>(l.xf_scale));
        xf.set("rotate", static_cast<double>(l.xf_rotate));
        v.set("transform", std::move(xf));
    }
    if (l.node_x != 0.0f || l.node_y != 0.0f) {
        v.set("node_x", static_cast<double>(l.node_x));
        v.set("node_y", static_cast<double>(l.node_y));
    }
    Value stack = Value::make_array();
    for (const EffectInstance& fx : l.stack) stack.push(effect_to_json(fx));
    v.set("stack", std::move(stack));
    Value groups = Value::make_array();
    for (const Group& g : l.groups) groups.push(group_to_json(g));
    v.set("groups", std::move(groups));
    return v;
}

Layer layer_from_json(const Value& v) {
    Layer l;
    l.id = static_cast<uint64_t>(v.get("id").as_int(0));
    l.name = v.get("name").as_string();
    std::string src_name = v.get("source").as_string();
    if (src_name == "clip") src_name = "media";   // pre-rename projects
    l.source = static_cast<LayerSourceKind>(
        enum_index(kSourceKindNames, src_name));
    l.asset = static_cast<uint64_t>(v.get("asset").as_int(0));
    l.slip = static_cast<uint32_t>(v.get("slip").as_int(0));
    l.timeline_lock = v.get("timeline_lock").as_bool(false);
    l.target = static_cast<uint64_t>(v.get("target").as_int(0));
    f3_from_json(v.get("color_a"), l.color_a);
    f3_from_json(v.get("color_b"), l.color_b);
    l.gen_scale = num(v, "gen_scale", 6.0f);
    l.gen_angle = num(v, "gen_angle", 0.0f);
    l.gen_phase = num(v, "gen_phase", 0.0f);
    l.osc_shape = static_cast<uint32_t>(v.get("osc_shape").as_int(0));
    if (const Value& pv = v.get("path"); pv.is_array()) {
        for (const Value& ptv : pv.array()) {
            if (!ptv.is_array()) continue;
            const Array& c = ptv.array();
            PathPoint p;
            if (c.size() >= 2) {
                p.ax = static_cast<float>(c[0].as_number(0.0));
                p.ay = static_cast<float>(c[1].as_number(0.0));
            }
            if (c.size() >= 6) {
                p.in_dx = static_cast<float>(c[2].as_number(0.0));
                p.in_dy = static_cast<float>(c[3].as_number(0.0));
                p.out_dx = static_cast<float>(c[4].as_number(0.0));
                p.out_dy = static_cast<float>(c[5].as_number(0.0));
            }
            l.path.push_back(p);
        }
        l.path_closed = !v.get("path_open").as_bool(false);
    }
    l.blend = static_cast<BlendMode>(
        enum_index(kBlendNames, v.get("blend").as_string()));
    l.opacity = num(v, "opacity", 1.0f);
    l.visible = v.get("visible").as_bool(true);
    if (const Value& xf = v.get("transform"); xf.is_object()) {
        l.crop_l = num(xf, "crop_l", 0.0f);
        l.crop_r = num(xf, "crop_r", 0.0f);
        l.crop_t = num(xf, "crop_t", 0.0f);
        l.crop_b = num(xf, "crop_b", 0.0f);
        l.flip_h = xf.get("flip_h").as_bool(false);
        l.flip_v = xf.get("flip_v").as_bool(false);
        l.xf_scale = num(xf, "scale", 1.0f);
        l.xf_rotate = num(xf, "rotate", 0.0f);
    }
    l.node_x = num(v, "node_x", 0.0f);
    l.node_y = num(v, "node_y", 0.0f);
    for (const Value& fv : v.get("stack").array())
        if (auto fx = effect_from_json(fv)) l.stack.push_back(std::move(*fx));
    for (const Value& gv : v.get("groups").array())
        l.groups.push_back(group_from_json(gv));
    return l;
}

// ---- assets

Value asset_to_json(const Asset& a) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(a.id));
    v.set("name", a.name);
    v.set("path", a.path);
    if (a.frame_count)
        v.set("frames", static_cast<int64_t>(a.frame_count));
    if (a.fps > 0.0) v.set("fps", a.fps);
    if (a.width && a.height) {
        v.set("width", static_cast<int64_t>(a.width));
        v.set("height", static_cast<int64_t>(a.height));
    }
    if (a.still_duration_frames)
        v.set("still_duration",
              static_cast<int64_t>(a.still_duration_frames));
    if (a.bin) v.set("bin", static_cast<int64_t>(a.bin));
    return v;
}

Asset asset_from_json(const Value& v) {
    Asset a;
    a.id = static_cast<uint64_t>(v.get("id").as_int(0));
    a.name = v.get("name").as_string();
    a.path = v.get("path").as_string();
    a.frame_count = static_cast<uint32_t>(v.get("frames").as_int(0));
    a.fps = v.get("fps").as_number(0.0);
    a.width = static_cast<uint32_t>(v.get("width").as_int(0));
    a.height = static_cast<uint32_t>(v.get("height").as_int(0));
    a.still_duration_frames =
        static_cast<uint32_t>(v.get("still_duration").as_int(0));
    a.bin = static_cast<uint64_t>(v.get("bin").as_int(0));
    return a;
}

// ---- looks

// In ports hold ONE producer — only the Output composites fan-in (the
// layer merge). Hand-edited files keep the LAST link per (to, port),
// matching connect's replace-on-connect.
void dedupe_links(std::vector<NodeLink>& links) {
    for (size_t i = links.size(); i-- > 0;) {
        const NodeLink& l = links[i];
        if (l.to == 0) continue;
        for (size_t j = i; j-- > 0;) {
            if (links[j].to == l.to && links[j].to_port == l.to_port) {
                links.erase(links.begin() + static_cast<ptrdiff_t>(j));
                --i;
            }
        }
    }
}

Value look_to_json(const Look& look) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(look.id));
    v.set("name", look.name);
    if (look.duration)
        v.set("duration", static_cast<int64_t>(look.duration));
    if (look.bin) v.set("bin", static_cast<int64_t>(look.bin));
    if (look.audio_split) v.set("audio_split", true);

    Value layers = Value::make_array();
    for (const Layer& l : look.layers) layers.push(layer_to_json(l));
    v.set("layers", std::move(layers));

    Value vnodes = Value::make_array();
    for (const ValueNode& n : look.value_nodes)
        vnodes.push(value_node_to_json(n));
    v.set("value_nodes", std::move(vnodes));

    Value routes = Value::make_array();
    for (const ModRoute& r : look.mod_routes) routes.push(route_to_json(r));
    v.set("mod_routes", std::move(routes));

    Value lanes = Value::make_array();
    for (const KeyframeLane& lane : look.lanes) {
        if (lane.keys.empty()) continue;
        lanes.push(lane_to_json(lane));
    }
    v.set("lanes", std::move(lanes));

    Value snapshots = Value::make_array();
    for (const Snapshot& s : look.snapshots)
        snapshots.push(snapshot_to_json(s));
    v.set("snapshots", std::move(snapshots));
    v.set("morph_from", look.morph_from);
    v.set("morph_to", look.morph_to);
    v.set("morph_pos", static_cast<double>(look.morph_pos));

    if (look.out_node_x != 0.0f || look.out_node_y != 0.0f) {
        v.set("out_node_x", static_cast<double>(look.out_node_x));
        v.set("out_node_y", static_cast<double>(look.out_node_y));
    }
    if (!look.links.empty()) {
        Value links = Value::make_array();
        for (const NodeLink& l : look.links) {
            Value lv = Value::make_object();
            lv.set("from", static_cast<int64_t>(l.from));
            lv.set("to", static_cast<int64_t>(l.to));
            lv.set("port", static_cast<int64_t>(l.to_port));
            links.push(std::move(lv));
        }
        v.set("links", std::move(links));
    }
    if (!look.frames.empty()) {
        Value frames = Value::make_array();
        for (const CanvasFrame& f : look.frames) {
            Value fv = Value::make_object();
            fv.set("id", static_cast<int64_t>(f.id));
            fv.set("x", static_cast<double>(f.x));
            fv.set("y", static_cast<double>(f.y));
            fv.set("w", static_cast<double>(f.w));
            fv.set("h", static_cast<double>(f.h));
            fv.set("title", f.title);
            if (f.color) fv.set("color", static_cast<int64_t>(f.color));
            frames.push(std::move(fv));
        }
        v.set("frames", std::move(frames));
    }
    return v;
}

Look look_from_json(const Value& v) {
    Look look;
    look.id = static_cast<uint64_t>(v.get("id").as_int(0));
    look.name = v.get("name").as_string();
    look.duration = static_cast<uint32_t>(v.get("duration").as_int(0));
    look.bin = static_cast<uint64_t>(v.get("bin").as_int(0));
    look.audio_split = v.get("audio_split").as_bool(false);

    for (const Value& lv : v.get("layers").array()) {
        if (look.layers.size() >= kMaxLayers) break;
        look.layers.push_back(layer_from_json(lv));
    }
    for (const Value& nv : v.get("value_nodes").array())
        look.value_nodes.push_back(value_node_from_json(nv));
    for (const Value& rv : v.get("mod_routes").array())
        look.mod_routes.push_back(route_from_json(rv));
    for (const Value& lv : v.get("lanes").array())
        look.lanes.push_back(lane_from_json(lv));
    const Array& snaps = v.get("snapshots").array();
    for (size_t i = 0; i < 3 && i < snaps.size(); ++i)
        look.snapshots[i] = snapshot_from_json(snaps[i]);
    look.morph_from = static_cast<int>(v.get("morph_from").as_int(0));
    look.morph_to = static_cast<int>(v.get("morph_to").as_int(1));
    look.morph_pos = num(v, "morph_pos", 0.0f);

    look.out_node_x = num(v, "out_node_x", 0.0f);
    look.out_node_y = num(v, "out_node_y", 0.0f);
    // An absent link table means implicit chain wiring; consumers call
    // ensure_links when they need the graph — the loader stays
    // byte-roundtrip-stable.
    for (const Value& lv : v.get("links").array())
        look.links.push_back(
            {static_cast<uint64_t>(lv.get("from").as_int(0)),
             static_cast<uint64_t>(lv.get("to").as_int(0)),
             static_cast<uint32_t>(lv.get("port").as_int(0))});
    dedupe_links(look.links);
    for (const Value& fv : v.get("frames").array()) {
        CanvasFrame f;
        f.id = static_cast<uint64_t>(fv.get("id").as_int(0));
        f.x = num(fv, "x", 0.0f);
        f.y = num(fv, "y", 0.0f);
        f.w = num(fv, "w", 480.0f);
        f.h = num(fv, "h", 360.0f);
        f.title = fv.get("title").as_string();
        f.color = static_cast<uint32_t>(fv.get("color").as_int(0));
        look.frames.push_back(std::move(f));
    }
    return look;
}

// Highest id in a look, so id counters can never mint a duplicate.
uint64_t max_node_id(const Look& look) {
    uint64_t max_id = look.id;
    for (const Layer& l : look.layers) {
        max_id = std::max(max_id, l.id);
        for (const EffectInstance& fx : l.stack)
            max_id = std::max(max_id, fx.id);
        for (const Group& g : l.groups) max_id = std::max(max_id, g.id);
    }
    for (const CanvasFrame& f : look.frames) max_id = std::max(max_id, f.id);
    return max_id;
}

uint64_t max_sequence_id(const Sequence& seq) {
    uint64_t max_id = seq.id;
    for (const SeqTrack& t : seq.tracks) {
        max_id = std::max(max_id, t.id);
        for (const Placement& p : t.placements)
            max_id = std::max(max_id, std::max(p.id, p.link));
    }
    for (const AudioTrack& t : seq.audio) {
        max_id = std::max(max_id, t.id);
        for (const Placement& p : t.placements)
            max_id = std::max(max_id, std::max(p.id, p.link));
    }
    return max_id;
}

// ---- sequences

Value sequence_to_json(const Sequence& seq) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(seq.id));
    v.set("name", seq.name);
    if (seq.duration)
        v.set("duration", static_cast<int64_t>(seq.duration));
    if (seq.bin) v.set("bin", static_cast<int64_t>(seq.bin));

    Value tracks = Value::make_array();
    for (const SeqTrack& t : seq.tracks) {
        Value tv = Value::make_object();
        tv.set("id", static_cast<int64_t>(t.id));
        tv.set("name", t.name);
        Value places = Value::make_array();
        for (const Placement& p : t.placements)
            places.push(placement_to_json(p));
        tv.set("placements", std::move(places));
        tracks.push(std::move(tv));
    }
    v.set("tracks", std::move(tracks));

    if (!seq.audio.empty()) {
        Value audio = Value::make_array();
        for (const AudioTrack& t : seq.audio) {
            Value tv = Value::make_object();
            tv.set("id", static_cast<int64_t>(t.id));
            tv.set("name", t.name);
            if (t.gain != 1.0f)
                tv.set("gain", static_cast<double>(t.gain));
            if (t.mute) tv.set("mute", true);
            Value places = Value::make_array();
            for (const Placement& p : t.placements)
                places.push(placement_to_json(p));
            tv.set("placements", std::move(places));
            audio.push(std::move(tv));
        }
        v.set("audio", std::move(audio));
    }

    if (seq.trim_in) v.set("trim_in", static_cast<int64_t>(seq.trim_in));
    if (seq.trim_out) v.set("trim_out", static_cast<int64_t>(seq.trim_out));
    if (seq.loop_out > seq.loop_in) {
        v.set("loop_in", static_cast<int64_t>(seq.loop_in));
        v.set("loop_out", static_cast<int64_t>(seq.loop_out));
    }
    if (!seq.markers.empty()) {
        Value markers = Value::make_array();
        for (const uint32_t m : seq.markers)
            markers.push(Value(static_cast<int64_t>(m)));
        v.set("markers", std::move(markers));
    }
    return v;
}

Sequence sequence_from_json(const Value& v) {
    Sequence seq;
    seq.id = static_cast<uint64_t>(v.get("id").as_int(0));
    seq.name = v.get("name").as_string();
    seq.duration = static_cast<uint32_t>(v.get("duration").as_int(0));
    seq.bin = static_cast<uint64_t>(v.get("bin").as_int(0));

    for (const Value& tv : v.get("tracks").array()) {
        if (seq.tracks.size() >= kMaxLayers) break;
        SeqTrack t;
        t.id = static_cast<uint64_t>(tv.get("id").as_int(0));
        t.name = tv.get("name").as_string();
        for (const Value& pl : tv.get("placements").array()) {
            if (t.placements.size() >= kMaxPlacementsPerTrack) break;
            t.placements.push_back(placement_from_json(pl));
        }
        seq.tracks.push_back(std::move(t));
    }
    for (const Value& tv : v.get("audio").array()) {
        if (seq.audio.size() >= kMaxLayers) break;
        AudioTrack t;
        t.id = static_cast<uint64_t>(tv.get("id").as_int(0));
        t.name = tv.get("name").as_string();
        t.gain = num(tv, "gain", 1.0f);
        t.mute = tv.get("mute").as_bool(false);
        for (const Value& pl : tv.get("placements").array()) {
            if (t.placements.size() >= kMaxPlacementsPerTrack) break;
            t.placements.push_back(placement_from_json(pl));
        }
        seq.audio.push_back(std::move(t));
    }

    seq.trim_in = static_cast<uint32_t>(v.get("trim_in").as_int(0));
    seq.trim_out = static_cast<uint32_t>(v.get("trim_out").as_int(0));
    seq.loop_in = static_cast<uint32_t>(v.get("loop_in").as_int(0));
    seq.loop_out = static_cast<uint32_t>(v.get("loop_out").as_int(0));
    for (const Value& mv : v.get("markers").array())
        seq.markers.push_back(static_cast<uint32_t>(mv.as_int(0)));
    std::sort(seq.markers.begin(), seq.markers.end());
    return seq;
}

}  // namespace

// ---- effects (shared with presets)

json::Value effect_to_json(const EffectInstance& fx) {
    Value v = Value::make_object();
    v.set("type", effect_info(fx.type).id);
    v.set("id", static_cast<int64_t>(fx.id));
    Array params;
    for (float p : fx.params) params.push_back(Value(static_cast<double>(p)));
    v.set("params", Value(std::move(params)));
    v.set("wet", static_cast<double>(fx.wet));
    v.set("opacity", static_cast<double>(fx.opacity));
    v.set("blend", enum_name(kBlendNames, static_cast<uint32_t>(fx.blend)));
    v.set("bypass", fx.bypass);
    if (fx.solo) v.set("solo", true);
    v.set("seed", static_cast<int64_t>(fx.seed));
    v.set("group", static_cast<int64_t>(fx.group_id));
    if (!fx.text.empty()) v.set("text", fx.text);
    if (fx.node_x != 0.0f || fx.node_y != 0.0f) {
        v.set("node_x", static_cast<double>(fx.node_x));
        v.set("node_y", static_cast<double>(fx.node_y));
    }
    return v;
}

std::optional<EffectInstance> effect_from_json(const json::Value& v) {
    const auto type = effect_type_from_id(v.get("type").as_string());
    if (!type) return std::nullopt;   // effect from a newer build: skip
    const EffectInfo& info = effect_info(*type);
    EffectInstance fx;
    fx.type = *type;
    fx.id = static_cast<uint64_t>(v.get("id").as_int(0));
    fx.params.reserve(info.param_count);
    for (uint32_t i = 0; i < info.param_count; ++i)
        fx.params.push_back(info.params[i].default_value);
    const Array& params = v.get("params").array();
    for (size_t i = 0; i < fx.params.size() && i < params.size(); ++i)
        fx.params[i] = static_cast<float>(params[i].as_number(fx.params[i]));
    fx.wet = num(v, "wet", 1.0f);
    fx.opacity = num(v, "opacity", 1.0f);
    fx.blend = static_cast<BlendMode>(
        enum_index(kBlendNames, v.get("blend").as_string()));
    fx.bypass = v.get("bypass").as_bool(false);
    fx.solo = v.get("solo").as_bool(false);
    fx.seed = static_cast<uint64_t>(v.get("seed").as_int(0));
    fx.group_id = static_cast<uint64_t>(v.get("group").as_int(0));
    fx.text = v.get("text").as_string();
    fx.node_x = num(v, "node_x", 0.0f);
    fx.node_y = num(v, "node_y", 0.0f);
    return fx;
}

// ---- groups (shared with presets)

json::Value group_to_json(const Group& g) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(g.id));
    v.set("name", g.name);
    v.set("folded", g.folded);
    v.set("bypass", g.bypass);
    if (g.node_x != 0.0f || g.node_y != 0.0f) {
        v.set("node_x", static_cast<double>(g.node_x));
        v.set("node_y", static_cast<double>(g.node_y));
    }
    // The group face: exposed member params — direct aliases.
    Value exposed = Value::make_array();
    for (const ParamKey& k : g.exposed) {
        Value ev = Value::make_object();
        ev.set("effect", static_cast<int64_t>(k.effect_id));
        ev.set("param", k.param_index);
        exposed.push(std::move(ev));
    }
    v.set("exposed", std::move(exposed));
    if (g.face_in) v.set("face_in", static_cast<int64_t>(g.face_in));
    if (g.face_out) v.set("face_out", static_cast<int64_t>(g.face_out));
    if (g.in_x != 0.0f || g.in_y != 0.0f) {
        v.set("in_x", static_cast<double>(g.in_x));
        v.set("in_y", static_cast<double>(g.in_y));
    }
    if (g.out_x != 0.0f || g.out_y != 0.0f) {
        v.set("out_x", static_cast<double>(g.out_x));
        v.set("out_y", static_cast<double>(g.out_y));
    }
    return v;
}

Group group_from_json(const json::Value& v) {
    Group g;
    g.id = static_cast<uint64_t>(v.get("id").as_int(0));
    g.name = v.get("name").as_string();
    g.folded = v.get("folded").as_bool(false);
    g.bypass = v.get("bypass").as_bool(false);
    g.node_x = num(v, "node_x", 0.0f);
    g.node_y = num(v, "node_y", 0.0f);
    for (const Value& ev : v.get("exposed").array())
        g.exposed.push_back(
            {static_cast<uint64_t>(ev.get("effect").as_int(0)),
             static_cast<int>(ev.get("param").as_int(0))});
    g.face_in = static_cast<uint64_t>(v.get("face_in").as_int(0));
    g.face_out = static_cast<uint64_t>(v.get("face_out").as_int(0));
    g.in_x = num(v, "in_x", 0.0f);
    g.in_y = num(v, "in_y", 0.0f);
    g.out_x = num(v, "out_x", 0.0f);
    g.out_y = num(v, "out_y", 0.0f);
    return g;
}

// ---- document

json::Value doc_to_json(const Document& doc) {
    Value v = Value::make_object();
    v.set("looks_project", kProjectVersion);
    v.set("name", doc.name);
    v.set("master_seed", static_cast<int64_t>(doc.master_seed));
    if (doc.fps > 0.0) v.set("fps", doc.fps);
    if (doc.canvas_w && doc.canvas_h) {
        v.set("canvas_w", static_cast<int64_t>(doc.canvas_w));
        v.set("canvas_h", static_cast<int64_t>(doc.canvas_h));
    }
    v.set("cache_mb", static_cast<int64_t>(doc.cache_mb));
    if (doc.use_proxy) v.set("use_proxy", true);
    v.set("next_effect_id", static_cast<int64_t>(doc.next_effect_id));
    v.set("next_route_id", static_cast<int64_t>(doc.next_route_id));

    Value assets = Value::make_array();
    for (const Asset& a : doc.assets) assets.push(asset_to_json(a));
    v.set("assets", std::move(assets));

    Value looks = Value::make_array();
    for (const Look& look : doc.looks) looks.push(look_to_json(look));
    v.set("looks", std::move(looks));

    v.set("root_sequence", static_cast<int64_t>(doc.root_sequence));
    Value sequences = Value::make_array();
    for (const Sequence& s : doc.sequences)
        sequences.push(sequence_to_json(s));
    v.set("sequences", std::move(sequences));

    if (!doc.bins.empty()) {
        Value bins = Value::make_array();
        for (const Bin& b : doc.bins) {
            Value bv = Value::make_object();
            bv.set("id", static_cast<int64_t>(b.id));
            bv.set("name", b.name);
            if (b.parent)
                bv.set("parent", static_cast<int64_t>(b.parent));
            bins.push(std::move(bv));
        }
        v.set("bins", std::move(bins));
    }

    v.set("speed", static_cast<double>(doc.speed));
    v.set("time_mode", static_cast<int64_t>(doc.time_mode));
    if (!doc.sidechain_path.empty()) {
        v.set("sidechain", doc.sidechain_path);
        v.set("sidechain_mux", doc.sidechain_mux);
    }
    if (doc.audio_offset_ms != 0.0f)
        v.set("audio_offset_ms", static_cast<double>(doc.audio_offset_ms));
    if (doc.export_bitrate_mbps != 8.0f)
        v.set("export_bitrate_mbps",
              static_cast<double>(doc.export_bitrate_mbps));
    if (doc.export_scale != 1)
        v.set("export_scale", static_cast<int64_t>(doc.export_scale));
    if (!doc.export_audio) v.set("export_audio", false);

    return v;
}

Document doc_from_json(const json::Value& v) {
    Document doc;
    doc.looks.clear();
    doc.sequences.clear();
    doc.name = v.get("name").as_string();
    if (doc.name.empty()) doc.name = "untitled";
    doc.master_seed = static_cast<uint64_t>(v.get("master_seed").as_int(0));
    doc.fps = v.get("fps").as_number(0.0);
    doc.canvas_w = static_cast<uint32_t>(v.get("canvas_w").as_int(0));
    doc.canvas_h = static_cast<uint32_t>(v.get("canvas_h").as_int(0));
    doc.cache_mb = static_cast<uint32_t>(v.get("cache_mb").as_int(2048));
    doc.use_proxy = v.get("use_proxy").as_bool(false);
    doc.speed = num(v, "speed", 1.0f);
    doc.time_mode = static_cast<uint32_t>(v.get("time_mode").as_int(0));
    doc.sidechain_path = v.get("sidechain").as_string();
    doc.sidechain_mux = v.get("sidechain_mux").as_bool(false);
    doc.audio_offset_ms = num(v, "audio_offset_ms", 0.0f);
    doc.export_bitrate_mbps =
        std::clamp(num(v, "export_bitrate_mbps", 8.0f), 1.0f, 60.0f);
    doc.export_scale = std::clamp(
        static_cast<uint32_t>(v.get("export_scale").as_int(1)), 1u, 4u);
    doc.export_audio = v.get("export_audio").as_bool(true);

    for (const Value& av : v.get("assets").array())
        doc.assets.push_back(asset_from_json(av));

    for (const Value& lv : v.get("looks").array()) {
        if (doc.looks.size() >= kMaxLooks) break;
        doc.looks.push_back(look_from_json(lv));
    }
    for (const Value& sv : v.get("sequences").array()) {
        if (doc.sequences.size() >= kMaxLooks) break;
        doc.sequences.push_back(sequence_from_json(sv));
    }
    doc.root_sequence =
        static_cast<uint64_t>(v.get("root_sequence").as_int(0));

    for (const Value& bv : v.get("bins").array()) {
        Bin b;
        b.id = static_cast<uint64_t>(bv.get("id").as_int(0));
        b.name = bv.get("name").as_string();
        b.parent = static_cast<uint64_t>(bv.get("parent").as_int(0));
        if (b.id) doc.bins.push_back(std::move(b));
    }
    // Bin refs heal to the root: a dangling parent or membership, or a
    // parent loop, must not orphan rows out of the browser.
    for (Bin& b : doc.bins)
        if (b.parent &&
            (!doc.find_bin(b.parent) || bin_reaches(doc, b.parent, b.id)))
            b.parent = 0;
    auto heal_bin = [&](uint64_t* slot) {
        if (*slot && !doc.find_bin(*slot)) *slot = 0;
    };
    for (Look& look : doc.looks) heal_bin(&look.bin);
    for (Sequence& seq : doc.sequences) heal_bin(&seq.bin);
    for (Asset& a : doc.assets) heal_bin(&a.bin);

    // Re-derive id counters from the content: stored values are honored but
    // never allowed below (max seen id + 1), so a hand-edited file cannot
    // mint duplicate ids.
    uint64_t max_id = 0, max_route_id = 0;
    for (const Look& look : doc.looks)
        max_id = std::max(max_id, max_node_id(look));
    for (const Sequence& seq : doc.sequences)
        max_id = std::max(max_id, max_sequence_id(seq));
    for (const Asset& a : doc.assets) max_id = std::max(max_id, a.id);
    for (const Bin& b : doc.bins) max_id = std::max(max_id, b.id);
    for (const Look& look : doc.looks) {
        for (const ValueNode& n : look.value_nodes)
            max_route_id = std::max(max_route_id, n.id);
        for (const ModRoute& r : look.mod_routes)
            max_route_id = std::max(max_route_id, r.id);
    }
    doc.next_effect_id =
        std::max(static_cast<uint64_t>(v.get("next_effect_id").as_int(1)),
                 max_id + 1);
    doc.next_route_id =
        std::max(static_cast<uint64_t>(v.get("next_route_id").as_int(1)),
                 max_route_id + 1);

    // A document always holds at least one look holding at least one
    // layer, and at least one sequence holding at least one lane.
    if (doc.looks.empty()) {
        Look look;
        look.name = "look 1";
        doc.looks.push_back(std::move(look));
    }
    for (Look& look : doc.looks) {
        if (!look.id) look.id = doc.next_effect_id++;
        if (look.layers.empty()) {
            Layer base;
            base.id = doc.next_effect_id++;
            base.name = "layer 1";
            look.layers.push_back(std::move(base));
        }
    }
    if (doc.sequences.empty()) {
        Sequence seq;
        seq.id = doc.next_effect_id++;
        seq.name = "sequence 1";
        doc.sequences.push_back(std::move(seq));
    }
    for (Sequence& seq : doc.sequences) {
        if (!seq.id) seq.id = doc.next_effect_id++;
        if (seq.tracks.empty()) {
            SeqTrack lane;
            lane.id = doc.next_effect_id++;
            lane.name = "v1";
            seq.tracks.push_back(std::move(lane));
        }
    }
    if (!doc.find_sequence(doc.root_sequence))
        doc.root_sequence = doc.sequences.front().id;

    // Assets and hand-authored entries minted after the id high-water
    // mark. An unbound placement is a deliberate state (a dormant block)
    // and loads back exactly as written.
    for (Asset& a : doc.assets)
        if (!a.id) a.id = doc.next_effect_id++;
    for (Look& look : doc.looks)
        for (Layer& l : look.layers)
            if (!l.id) l.id = doc.next_effect_id++;
    for (Sequence& seq : doc.sequences) {
        for (SeqTrack& t : seq.tracks) {
            if (!t.id) t.id = doc.next_effect_id++;
            for (Placement& p : t.placements)
                if (!p.id) p.id = doc.next_effect_id++;
        }
        for (AudioTrack& t : seq.audio) {
            if (!t.id) t.id = doc.next_effect_id++;
            for (Placement& p : t.placements)
                if (!p.id) p.id = doc.next_effect_id++;
        }
    }
    return doc;
}

bool save_document(const std::filesystem::path& path, const Document& doc) {
    const std::string text = json::write(doc_to_json(doc), /*pretty=*/true);
    return write_file_bytes(path, text.data(), text.size());
}

std::optional<Document> load_document(const std::filesystem::path& path,
                                      std::string* error) {
    auto bytes = read_file_bytes(path);
    if (!bytes) {
        if (error) *error = "cannot read file";
        return std::nullopt;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()),
                                bytes->size());
    json::ParseResult parsed = json::parse(text);
    if (!parsed.value) {
        if (error) *error = parsed.error;
        return std::nullopt;
    }
    if (!parsed.value->is_object() ||
        parsed.value->get("looks_project").as_int(0) < 1) {
        if (error) *error = "not a looks project file";
        return std::nullopt;
    }
    // Clean break at version 5 (value graph): an older file's inline
    // route sources would load as silence, which reads as data loss.
    // Refuse it honestly.
    if (parsed.value->get("looks_project").as_int(0) < kProjectVersion) {
        if (error) *error = "project predates the value graph format";
        return std::nullopt;
    }
    return doc_from_json(*parsed.value);
}

}  // namespace looks::doc
