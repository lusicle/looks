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
const char* const kSourceKindNames[] = {"clip", "solid", "gradient", "noise",
                                        "test", "adjustment"};
const char* const kModSourceNames[] = {
    "lfo",         "drift",       "audio_low",    "audio_mid",
    "audio_high",  "audio_onset", "video_motion", "video_brightness",
    "lfo_beat",    "envelope",    "video_cut",    "beat"};
const char* const kLfoShapeNames[] = {"sine", "triangle", "square",
                                      "sample_hold"};
const char* const kCurveNames[] = {"linear", "exp", "scurve", "inverted"};
const char* const kMaskTypeNames[] = {"shape", "luma", "luma_key",
                                      "chroma_key", "motion"};
const char* const kMaskExtractNames[] = {"luma", "red", "green", "blue",
                                         "alpha"};
const char* const kMaskCombineNames[] = {"add", "subtract", "intersect"};

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
    // Mask keys carry bit 63, past the JSON number's 2^53 exact-integer
    // range — store the bare mask id in its own field instead.
    if (k.effect_id & kMaskParamBit)
        v.set("mask", static_cast<int64_t>(k.effect_id & ~kMaskParamBit));
    else
        v.set("effect", static_cast<int64_t>(k.effect_id));
    v.set("param", k.param_index);
    return v;
}

ParamKey param_key_from_json(const Value& v) {
    ParamKey k;
    const int64_t mask_id = v.get("mask").as_int(-1);
    k.effect_id = mask_id >= 0
        ? static_cast<uint64_t>(mask_id) | kMaskParamBit
        : static_cast<uint64_t>(v.get("effect").as_int(0));
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
    return s;
}

Value route_to_json(const ModRoute& r) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(r.id));
    v.set("source", mod_source_to_json(r.source));
    v.set("target", param_key_to_json(r.target));
    v.set("amount", static_cast<double>(r.amount));
    v.set("curve", enum_name(kCurveNames, static_cast<uint32_t>(r.curve)));
    return v;
}

ModRoute route_from_json(const Value& v) {
    ModRoute r;
    r.id = static_cast<uint64_t>(v.get("id").as_int(0));
    r.source = mod_source_from_json(v.get("source"));
    r.target = param_key_from_json(v.get("target"));
    r.amount = num(v, "amount", 0.0f);
    r.curve = static_cast<ResponseCurve>(
        enum_index(kCurveNames, v.get("curve").as_string()));
    return r;
}

Value lane_to_json(const KeyframeLane& lane) {
    Value v = Value::make_object();
    v.set("target", param_key_to_json(lane.target));
    if (lane.loop) v.set("loop", true);
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

// ---- masks

Value mask_to_json(const Mask& m) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(m.id));
    v.set("name", m.name);
    v.set("type", enum_name(kMaskTypeNames, static_cast<uint32_t>(m.type)));
    v.set("center", Value(Array{Value(static_cast<double>(m.center_x)),
                                Value(static_cast<double>(m.center_y))}));
    v.set("radius", Value(Array{Value(static_cast<double>(m.radius_x)),
                                Value(static_cast<double>(m.radius_y))}));
    v.set("roundness", static_cast<double>(m.roundness));
    v.set("feather", static_cast<double>(m.feather));
    if (!m.points.empty()) {
        Value pts = Value::make_array();
        for (float p : m.points) pts.push(static_cast<double>(p));
        v.set("points", std::move(pts));
    }
    v.set("extract",
          enum_name(kMaskExtractNames, static_cast<uint32_t>(m.extract)));
    v.set("key_center", static_cast<double>(m.key_center));
    v.set("key_range", static_cast<double>(m.key_range));
    v.set("key_rgb", Value(Array{Value(static_cast<double>(m.key_r)),
                                 Value(static_cast<double>(m.key_g)),
                                 Value(static_cast<double>(m.key_b))}));
    v.set("blur_px", static_cast<double>(m.blur_px));
    v.set("black_point", static_cast<double>(m.black_point));
    v.set("white_point", static_cast<double>(m.white_point));
    v.set("gamma", static_cast<double>(m.gamma));
    v.set("invert", m.invert);
    v.set("source", m.source_path);
    if (m.source_layer_id)
        v.set("source_layer", static_cast<int64_t>(m.source_layer_id));
    if (m.source_gen) v.set("source_gen", static_cast<int64_t>(m.source_gen));
    v.set("gen_scale", static_cast<double>(m.gen_scale));
    v.set("gen_angle", static_cast<double>(m.gen_angle));
    v.set("free_run", m.free_run);
    v.set("fit", static_cast<int64_t>(m.fit));
    if (m.grow_px != 0.0f) v.set("grow_px", static_cast<double>(m.grow_px));
    if (m.combine_id) {
        v.set("combine", static_cast<int64_t>(m.combine_id));
        v.set("combine_op",
              enum_name(kMaskCombineNames,
                        static_cast<uint32_t>(m.combine_op)));
    }
    Value chain = Value::make_array();
    for (const EffectInstance& fx : m.chain) chain.push(effect_to_json(fx));
    v.set("chain", std::move(chain));
    return v;
}

Mask mask_from_json(const Value& v) {
    Mask m;
    m.id = static_cast<uint64_t>(v.get("id").as_int(0));
    m.name = v.get("name").as_string();
    m.type = static_cast<MaskType>(
        enum_index(kMaskTypeNames, v.get("type").as_string()));
    const Array& c = v.get("center").array();
    if (c.size() >= 2) {
        m.center_x = static_cast<float>(c[0].as_number(0.5));
        m.center_y = static_cast<float>(c[1].as_number(0.5));
    }
    const Array& r = v.get("radius").array();
    if (r.size() >= 2) {
        m.radius_x = static_cast<float>(r[0].as_number(0.3));
        m.radius_y = static_cast<float>(r[1].as_number(0.3));
    }
    m.roundness = num(v, "roundness", 1.0f);
    m.feather = num(v, "feather", 0.05f);
    for (const Value& p : v.get("points").array())
        m.points.push_back(static_cast<float>(p.as_number(0.0)));
    if (m.points.size() % 2) m.points.pop_back();
    m.extract = static_cast<MaskExtract>(
        enum_index(kMaskExtractNames, v.get("extract").as_string()));
    m.key_center = num(v, "key_center", 0.5f);
    m.key_range = num(v, "key_range", 0.25f);
    const Array& k = v.get("key_rgb").array();
    if (k.size() >= 3) {
        m.key_r = static_cast<float>(k[0].as_number(0.0));
        m.key_g = static_cast<float>(k[1].as_number(1.0));
        m.key_b = static_cast<float>(k[2].as_number(0.0));
    }
    m.blur_px = num(v, "blur_px", 0.0f);
    m.black_point = num(v, "black_point", 0.0f);
    m.white_point = num(v, "white_point", 1.0f);
    m.gamma = num(v, "gamma", 1.0f);
    m.invert = v.get("invert").as_bool(false);
    m.source_path = v.get("source").as_string();
    m.source_layer_id =
        static_cast<uint64_t>(v.get("source_layer").as_int(0));
    m.source_gen = static_cast<uint32_t>(v.get("source_gen").as_int(0));
    m.gen_scale = num(v, "gen_scale", 24.0f);
    m.gen_angle = num(v, "gen_angle", 0.0f);
    m.free_run = v.get("free_run").as_bool(false);
    m.fit = static_cast<uint32_t>(v.get("fit").as_int(0));
    m.grow_px = num(v, "grow_px", 0.0f);
    m.combine_id = static_cast<uint64_t>(v.get("combine").as_int(0));
    m.combine_op = static_cast<MaskCombineOp>(
        enum_index(kMaskCombineNames, v.get("combine_op").as_string()));
    for (const Value& fv : v.get("chain").array())
        if (auto fx = effect_from_json(fv)) m.chain.push_back(std::move(*fx));
    return m;
}

// ---- layers

Value layer_to_json(const Layer& l) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(l.id));
    v.set("name", l.name);
    v.set("source",
          enum_name(kSourceKindNames, static_cast<uint32_t>(l.source)));
    v.set("color_a", f3_to_json(l.color_a));
    v.set("color_b", f3_to_json(l.color_b));
    v.set("gen_scale", static_cast<double>(l.gen_scale));
    v.set("gen_angle", static_cast<double>(l.gen_angle));
    v.set("blend", enum_name(kBlendNames, static_cast<uint32_t>(l.blend)));
    v.set("opacity", static_cast<double>(l.opacity));
    v.set("visible", l.visible);
    if (l.mask_id) v.set("mask", static_cast<int64_t>(l.mask_id));
    // Transform + trim (spec §5): written only when non-default so
    // pre-transform projects stay byte-stable.
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
    if (l.trim_in > 0) v.set("trim_in", static_cast<int64_t>(l.trim_in));
    if (l.trim_out > 0) v.set("trim_out", static_cast<int64_t>(l.trim_out));
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
    l.source = static_cast<LayerSourceKind>(
        enum_index(kSourceKindNames, v.get("source").as_string()));
    f3_from_json(v.get("color_a"), l.color_a);
    f3_from_json(v.get("color_b"), l.color_b);
    l.gen_scale = num(v, "gen_scale", 6.0f);
    l.gen_angle = num(v, "gen_angle", 0.0f);
    l.blend = static_cast<BlendMode>(
        enum_index(kBlendNames, v.get("blend").as_string()));
    l.opacity = num(v, "opacity", 1.0f);
    l.visible = v.get("visible").as_bool(true);
    l.mask_id = static_cast<uint64_t>(v.get("mask").as_int(0));
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
    l.trim_in = static_cast<uint32_t>(v.get("trim_in").as_int(0));
    l.trim_out = static_cast<uint32_t>(v.get("trim_out").as_int(0));
    for (const Value& fv : v.get("stack").array())
        if (auto fx = effect_from_json(fv)) l.stack.push_back(std::move(*fx));
    for (const Value& gv : v.get("groups").array())
        l.groups.push_back(group_from_json(gv));
    return l;
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
    v.set("mask", static_cast<int64_t>(fx.mask_id));
    v.set("group", static_cast<int64_t>(fx.group_id));
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
    fx.mask_id = static_cast<uint64_t>(v.get("mask").as_int(0));
    fx.group_id = static_cast<uint64_t>(v.get("group").as_int(0));
    return fx;
}

// ---- groups (shared with presets)

json::Value group_to_json(const Group& g) {
    Value v = Value::make_object();
    v.set("id", static_cast<int64_t>(g.id));
    v.set("name", g.name);
    v.set("folded", g.folded);
    v.set("bypass", g.bypass);
    Value macros = Value::make_array();
    for (const MacroKnob& knob : g.macros) {
        Value kv = Value::make_object();
        kv.set("name", knob.name);
        kv.set("value", static_cast<double>(knob.value));
        Value targets = Value::make_array();
        for (const MacroTarget& t : knob.targets) {
            Value tv = Value::make_object();
            tv.set("effect", static_cast<int64_t>(t.effect_id));
            tv.set("param", t.param_index);
            tv.set("lo", static_cast<double>(t.lo));
            tv.set("hi", static_cast<double>(t.hi));
            tv.set("curve",
                   enum_name(kCurveNames, static_cast<uint32_t>(t.curve)));
            targets.push(std::move(tv));
        }
        kv.set("targets", std::move(targets));
        macros.push(std::move(kv));
    }
    v.set("macros", std::move(macros));
    return v;
}

Group group_from_json(const json::Value& v) {
    Group g;
    g.id = static_cast<uint64_t>(v.get("id").as_int(0));
    g.name = v.get("name").as_string();
    g.folded = v.get("folded").as_bool(false);
    g.bypass = v.get("bypass").as_bool(false);
    for (const Value& kv : v.get("macros").array()) {
        MacroKnob knob;
        knob.name = kv.get("name").as_string();
        knob.value = num(kv, "value", 0.0f);
        for (const Value& tv : kv.get("targets").array()) {
            MacroTarget t;
            t.effect_id = static_cast<uint64_t>(tv.get("effect").as_int(0));
            t.param_index = static_cast<int>(tv.get("param").as_int(0));
            t.lo = num(tv, "lo", 0.0f);
            t.hi = num(tv, "hi", 0.5f);
            t.curve = static_cast<ResponseCurve>(
                enum_index(kCurveNames, tv.get("curve").as_string()));
            knob.targets.push_back(t);
        }
        g.macros.push_back(std::move(knob));
    }
    return g;
}

// ---- document

json::Value doc_to_json(const Document& doc) {
    Value v = Value::make_object();
    v.set("looks_project", kProjectVersion);
    v.set("name", doc.name);
    v.set("clip", doc.clip_path);
    v.set("master_seed", static_cast<int64_t>(doc.master_seed));
    v.set("cache_mb", static_cast<int64_t>(doc.cache_mb));
    if (doc.use_proxy) v.set("use_proxy", true);
    v.set("next_effect_id", static_cast<int64_t>(doc.next_effect_id));
    v.set("next_route_id", static_cast<int64_t>(doc.next_route_id));
    v.set("next_mask_id", static_cast<int64_t>(doc.next_mask_id));

    Value layers = Value::make_array();
    for (const Layer& l : doc.layers) layers.push(layer_to_json(l));
    v.set("layers", std::move(layers));

    Value routes = Value::make_array();
    for (const ModRoute& r : doc.mod_routes) routes.push(route_to_json(r));
    v.set("mod_routes", std::move(routes));

    Value lanes = Value::make_array();
    for (const KeyframeLane& lane : doc.lanes) {
        if (lane.keys.empty()) continue;
        lanes.push(lane_to_json(lane));
    }
    v.set("lanes", std::move(lanes));

    Value snapshots = Value::make_array();
    for (const Snapshot& s : doc.snapshots) snapshots.push(snapshot_to_json(s));
    v.set("snapshots", std::move(snapshots));
    v.set("morph_from", doc.morph_from);
    v.set("morph_to", doc.morph_to);
    v.set("morph_pos", static_cast<double>(doc.morph_pos));
    v.set("speed", static_cast<double>(doc.speed));
    v.set("time_mode", static_cast<int64_t>(doc.time_mode));
    if (doc.clip_trim_in)
        v.set("clip_trim_in", static_cast<int64_t>(doc.clip_trim_in));
    if (doc.clip_trim_out)
        v.set("clip_trim_out", static_cast<int64_t>(doc.clip_trim_out));
    if (doc.loop_out > doc.loop_in) {
        v.set("loop_in", static_cast<int64_t>(doc.loop_in));
        v.set("loop_out", static_cast<int64_t>(doc.loop_out));
    }
    if (doc.still_duration_frames)
        v.set("still_duration",
              static_cast<int64_t>(doc.still_duration_frames));
    if (!doc.sidechain_path.empty()) {
        v.set("sidechain", doc.sidechain_path);
        v.set("sidechain_mux", doc.sidechain_mux);
    }
    if (doc.audio_offset_ms != 0.0f)
        v.set("audio_offset_ms", static_cast<double>(doc.audio_offset_ms));

    Value masks = Value::make_array();
    for (const Mask& m : doc.masks) masks.push(mask_to_json(m));
    v.set("masks", std::move(masks));
    return v;
}

Document doc_from_json(const json::Value& v) {
    Document doc;
    doc.layers.clear();
    doc.name = v.get("name").as_string();
    if (doc.name.empty()) doc.name = "untitled";
    doc.clip_path = v.get("clip").as_string();
    doc.master_seed = static_cast<uint64_t>(v.get("master_seed").as_int(0));
    doc.cache_mb = static_cast<uint32_t>(v.get("cache_mb").as_int(2048));
    doc.use_proxy = v.get("use_proxy").as_bool(false);

    for (const Value& lv : v.get("layers").array()) {
        if (doc.layers.size() >= kMaxLayers) break;
        doc.layers.push_back(layer_from_json(lv));
    }
    for (const Value& rv : v.get("mod_routes").array())
        doc.mod_routes.push_back(route_from_json(rv));
    for (const Value& lv : v.get("lanes").array())
        doc.lanes.push_back(lane_from_json(lv));
    const Array& snaps = v.get("snapshots").array();
    for (size_t i = 0; i < 3 && i < snaps.size(); ++i)
        doc.snapshots[i] = snapshot_from_json(snaps[i]);
    doc.morph_from = static_cast<int>(v.get("morph_from").as_int(0));
    doc.morph_to = static_cast<int>(v.get("morph_to").as_int(1));
    doc.morph_pos = num(v, "morph_pos", 0.0f);
    doc.speed = num(v, "speed", 1.0f);
    doc.time_mode = static_cast<uint32_t>(v.get("time_mode").as_int(0));
    doc.clip_trim_in = static_cast<uint32_t>(v.get("clip_trim_in").as_int(0));
    doc.clip_trim_out =
        static_cast<uint32_t>(v.get("clip_trim_out").as_int(0));
    doc.loop_in = static_cast<uint32_t>(v.get("loop_in").as_int(0));
    doc.loop_out = static_cast<uint32_t>(v.get("loop_out").as_int(0));
    doc.still_duration_frames =
        static_cast<uint32_t>(v.get("still_duration").as_int(0));
    doc.sidechain_path = v.get("sidechain").as_string();
    doc.sidechain_mux = v.get("sidechain_mux").as_bool(false);
    doc.audio_offset_ms = num(v, "audio_offset_ms", 0.0f);
    for (const Value& mv : v.get("masks").array())
        doc.masks.push_back(mask_from_json(mv));

    // Re-derive id counters from the content: stored values are honored but
    // never allowed below (max seen id + 1), so a hand-edited file cannot
    // mint duplicate ids.
    uint64_t max_effect_id = 0, max_mask_id = 0, max_route_id = 0;
    auto see_stack = [&](const std::vector<EffectInstance>& stack) {
        for (const EffectInstance& fx : stack)
            max_effect_id = std::max(max_effect_id, fx.id);
    };
    for (const Layer& l : doc.layers) {
        max_effect_id = std::max(max_effect_id, l.id);
        see_stack(l.stack);
        for (const Group& g : l.groups)
            max_effect_id = std::max(max_effect_id, g.id);
    }
    for (const Mask& m : doc.masks) {
        max_mask_id = std::max(max_mask_id, m.id);
        see_stack(m.chain);
    }
    for (const ModRoute& r : doc.mod_routes)
        max_route_id = std::max(max_route_id, r.id);
    doc.next_effect_id =
        std::max(static_cast<uint64_t>(v.get("next_effect_id").as_int(1)),
                 max_effect_id + 1);
    doc.next_route_id =
        std::max(static_cast<uint64_t>(v.get("next_route_id").as_int(1)),
                 max_route_id + 1);
    doc.next_mask_id =
        std::max(static_cast<uint64_t>(v.get("next_mask_id").as_int(1)),
                 max_mask_id + 1);

    if (doc.layers.empty()) {
        Layer base;
        base.id = doc.next_effect_id++;
        base.name = "layer 1";
        doc.layers.push_back(std::move(base));
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
    return doc_from_json(*parsed.value);
}

}  // namespace looks::doc
