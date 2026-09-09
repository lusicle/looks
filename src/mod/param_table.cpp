#include "mod/param_table.h"

#include <climits>

#include "doc/effects.h"
#include "doc/stack_commands.h"

namespace looks::mod {

const doc::EffectInstance* find_effect(const doc::Look& look,
                                       uint64_t effect_id, size_t* index_out) {
    const auto* fx = doc::find_effect(look, effect_id);
    if (fx && index_out) {
        *index_out = static_cast<size_t>(fx - look.effects.data());
    }
    return fx;
}

const doc::EffectInstance* find_effect(const doc::Document& doc,
                                       uint64_t effect_id, uint64_t* look_out,
                                       size_t* index_out) {
    for (const doc::Look& look : doc.looks) {
        if (const doc::EffectInstance* fx =
                find_effect(look, effect_id, index_out)) {
            if (look_out) *look_out = look.id;
            return fx;
        }
    }
    return nullptr;
}

void param_range(doc::EffectType type, int param_index, float* min_value,
                 float* max_value) {
    if (param_index < 0) {   // wet / opacity
        *min_value = 0.0f;
        *max_value = 1.0f;
        return;
    }
    const doc::EffectInfo& info = doc::effect_info(type);
    if (static_cast<uint32_t>(param_index) < info.param_count) {
        *min_value = info.params[param_index].min_value;
        *max_value = info.params[param_index].max_value;
    } else {
        *min_value = 0.0f;
        *max_value = 1.0f;
    }
}

bool param_discrete(doc::EffectType type, int param_index) {
    if (param_index < 0) return false;   // wet / opacity are continuous
    const doc::EffectInfo& info = doc::effect_info(type);
    return static_cast<uint32_t>(param_index) < info.param_count &&
           doc::param_is_discrete(info.params[param_index]);
}

float param_value(const doc::EffectInstance& fx, int param_index) {
    if (param_index == doc::kWetParam) return fx.wet;
    if (param_index == doc::kOpacityParam) return fx.opacity;
    if (param_index >= 0 && static_cast<size_t>(param_index) < fx.params.size())
        return fx.params[static_cast<size_t>(param_index)];
    return 0.0f;
}

float* source_param_slot(doc::Source& layer, int param_index) {
    switch (param_index) {
        case 0: return &layer.opacity;
        case 1: return &layer.color_a[0];
        case 2: return &layer.color_a[1];
        case 3: return &layer.color_a[2];
        case 4: return &layer.color_b[0];
        case 5: return &layer.color_b[1];
        case 6: return &layer.color_b[2];
        case 7: return &layer.gen_scale;
        case 8: return &layer.gen_angle;
        case 9: return &layer.crop_l;
        case 10: return &layer.crop_r;
        case 11: return &layer.crop_t;
        case 12: return &layer.crop_b;
        case 13: return &layer.xf_scale;
        case 14: return &layer.xf_rotate;
        // 15 slip / 16 waveform: field ids only, never modulatable.
        case 17: return &layer.gen_phase;
        case 18: return &layer.xf_anchor_x;
        case 19: return &layer.xf_anchor_y;
        // 20 gradient kind / 21 space: field ids only, never modulatable.
        case 22: return &layer.gradient_len;
        case 23: return &layer.gradient_x;
        case 24: return &layer.gradient_y;
        case 25: return &layer.color_a[3];
        case 26: return &layer.color_b[3];
        default: return nullptr;
    }
}

void source_param_range(int param_index, float* min_value, float* max_value) {
    *min_value = 0.0f;
    *max_value = 1.0f;
    switch (param_index) {
        case 7: *min_value = 1.0f; *max_value = 64.0f; break;
        case 8:
            *min_value = -3.14159265f;
            *max_value = 3.14159265f;
            break;
        case 9:
        case 10:
        case 11:
        case 12: *max_value = 0.45f; break;
        case 13: *min_value = 0.25f; *max_value = 4.0f; break;
        case 14: *min_value = -180.0f; *max_value = 180.0f; break;
        // Phase wraps in the kernel; the large max lets loops run many periods.
        case 17: *max_value = 1.0e6f; break;
        case 22: *min_value = 0.05f; *max_value = 4.0f; break;
        default: break;   // opacity + colors, normalized 0..1
    }
}

namespace {

constexpr const char* kSourceParamIds[doc::kSourceParamCount] = {
    "opacity", "color_a.r", "color_a.g", "color_a.b", "color_b.r",
    "color_b.g", "color_b.b", "scale",   "angle",     "crop_l",
    "crop_r",  "crop_t",    "crop_b",    "xf_scale",  "xf_rotate",
    "slip",    "waveform",  "phase",     "xf_anchor_x", "xf_anchor_y",
    "grad_kind", "grad_space", "grad_len", "grad_x", "grad_y",
    "color_a.a", "color_b.a"};

constexpr const char* kSourceParamLabels[doc::kSourceParamCount] = {
    "opacity", "color a r", "color a g", "color a b", "color b r",
    "color b g", "color b b", "scale",   "angle",     "crop left",
    "crop right", "crop top", "crop bottom", "transform scale",
    "transform rotate", "slip", "waveform", "phase", "anchor x",
    "anchor y", "gradient shape", "gradient blend", "gradient length",
    "gradient x", "gradient y", "color a alpha", "color b alpha"};

}  // namespace

int source_param_index_of(const std::string& id) {
    for (int i = 0; i < doc::kSourceParamCount; ++i)
        if (id == kSourceParamIds[i]) return i;
    return INT_MIN;
}

std::vector<ParamEntry> build_param_table(const doc::Document& doc,
                                          const doc::Look& look) {
    (void)doc;
    std::vector<ParamEntry> table;
    {
        // Look morph position: ParamKey {0, 0}.
        ParamEntry e;
        e.key = {0, 0};
        e.path = "global.morph";
        e.label = "morph position";
        e.min_value = 0.0f;
        e.max_value = 1.0f;
        e.base = look.morph_pos;
        table.push_back(std::move(e));
    }
    for (size_t i = 0; i < look.effects.size(); ++i) {
        const doc::EffectInstance& fx = look.effects[i];
        const doc::EffectInfo& info = doc::effect_info(fx.type);
        const std::string prefix = "fx" + std::to_string(fx.id) + ".";

        auto add = [&](int param_index, const char* id, const char* label,
                       float value) {
            ParamEntry e;
            e.key = {fx.id, param_index};
            e.path = prefix + id;
            e.label = std::string(info.label) + " " + label;
            param_range(fx.type, param_index, &e.min_value, &e.max_value);
            e.base = value;
            table.push_back(std::move(e));
        };

        add(doc::kWetParam, "wet", "wet/dry", fx.wet);
        add(doc::kOpacityParam, "opacity", "opacity", fx.opacity);
        for (uint32_t p = 0; p < info.param_count; ++p)
            add(static_cast<int>(p), info.params[p].id, info.params[p].label,
                fx.params[p]);
    }
    for (const doc::Group& g : look.groups) {
        const std::string gname = g.name.empty() ? "group" : g.name;
        auto add = [&](int param_index, const char* id, const char* label, float value) {
            ParamEntry e;
            e.key = {g.id | doc::kGroupParamBit, param_index};
            e.path = "group" + std::to_string(g.id) + "." + id;
            e.label = gname + " " + label;
            e.min_value = 0.0f;
            e.max_value = 1.0f;
            e.base = value;
            table.push_back(std::move(e));
        };
        add(doc::kWetParam, "wet", "wet/dry", g.wet);
        add(doc::kOpacityParam, "opacity", "opacity", g.opacity);
    }
    for (size_t l = 0; l < look.sources.size(); ++l) {
        doc::Source probe = look.sources[l];
        const std::string lname =
            probe.name.empty() ? "source " + std::to_string(probe.id) : probe.name;
        for (int p = 0; p < doc::kSourceParamCount; ++p) {
            float* slot = source_param_slot(probe, p);
            if (!slot) continue;
            ParamEntry e;
            e.key = {probe.id | doc::kSourceParamBit, p};
            e.path = "source" + std::to_string(probe.id) + "." + kSourceParamIds[p];
            e.label = lname + " " + kSourceParamLabels[p];
            source_param_range(p, &e.min_value, &e.max_value);
            e.base = *slot;
            table.push_back(std::move(e));
        }
    }
    return table;
}

}  // namespace looks::mod
