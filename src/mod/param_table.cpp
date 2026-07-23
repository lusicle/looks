#include "mod/param_table.h"

#include "doc/effects.h"
#include "doc/stack_commands.h"

namespace looks::mod {

const doc::EffectInstance* find_effect(const doc::Document& doc,
                                       uint64_t effect_id, size_t* layer_out,
                                       size_t* index_out) {
    for (size_t l = 0; l < doc.layers.size(); ++l) {
        const auto& stack = doc.layers[l].stack;
        for (size_t i = 0; i < stack.size(); ++i) {
            if (stack[i].id == effect_id) {
                if (layer_out) *layer_out = l;
                if (index_out) *index_out = i;
                return &stack[i];
            }
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

float param_value(const doc::EffectInstance& fx, int param_index) {
    if (param_index == doc::kWetParam) return fx.wet;
    if (param_index == doc::kOpacityParam) return fx.opacity;
    if (param_index >= 0 && static_cast<size_t>(param_index) < fx.params.size())
        return fx.params[static_cast<size_t>(param_index)];
    return 0.0f;
}

float* mask_param_slot(doc::Mask& mask, int param_index) {
    if (param_index >= doc::kMaskPointParamBase) {
        const size_t slot =
            static_cast<size_t>(param_index - doc::kMaskPointParamBase);
        return slot < mask.points.size() ? &mask.points[slot] : nullptr;
    }
    switch (param_index) {
        case 0: return &mask.feather;
        case 1: return &mask.center_x;
        case 2: return &mask.center_y;
        case 3: return &mask.radius_x;
        case 4: return &mask.radius_y;
        case 5: return &mask.blur_px;
        case 6: return &mask.black_point;
        case 7: return &mask.white_point;
        case 8: return &mask.gamma;
        case 9: return &mask.key_center;
        case 10: return &mask.key_range;
        default: return nullptr;
    }
}

void mask_param_range(int param_index, float* min_value, float* max_value) {
    *min_value = 0.0f;
    *max_value = 1.0f;
    switch (param_index) {
        case 0: *max_value = 0.5f; break;    // feather
        case 5: *max_value = 64.0f; break;   // blur_px
        case 8: *min_value = 0.1f; *max_value = 4.0f; break;   // gamma
        default: break;                      // normalized 0..1 fields
    }
}

namespace {

constexpr const char* kMaskParamIds[] = {
    "feather",     "center_x", "center_y", "radius_x",   "radius_y",
    "blur",        "black",    "white",    "gamma",      "key_center",
    "key_range"};

}  // namespace

std::vector<ParamEntry> build_param_table(const doc::Document& doc) {
    std::vector<ParamEntry> table;
    {
        // Global morph position (spec §7): ParamKey {0, 0}.
        ParamEntry e;
        e.key = {0, 0};
        e.path = "global.morph";
        e.label = "morph position";
        e.min_value = 0.0f;
        e.max_value = 1.0f;
        e.base = doc.morph_pos;
        table.push_back(std::move(e));
    }
    {
        // Global playback speed (spec §6.1): ParamKey {0, 1}.
        ParamEntry e;
        e.key = {0, 1};
        e.path = "global.speed";
        e.label = "speed";
        e.min_value = 0.0f;
        e.max_value = doc::kMaxSpeed;
        e.base = doc.speed;
        table.push_back(std::move(e));
    }
    for (size_t l = 0; l < doc.layers.size(); ++l)
    for (size_t i = 0; i < doc.layers[l].stack.size(); ++i) {
        const doc::EffectInstance& fx = doc.layers[l].stack[i];
        const doc::EffectInfo& info = doc::effect_info(fx.type);
        const std::string prefix = "layer" + std::to_string(l) + ".fx" +
                                   std::to_string(i) + ".";

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
    // Mask params (spec §8): mod targets like everything else.
    for (const doc::Mask& mask : doc.masks) {
        doc::Mask probe = mask;
        for (int p = 0; p < 11; ++p) {
            float* slot = mask_param_slot(probe, p);
            if (!slot) continue;
            ParamEntry e;
            e.key = {mask.id | doc::kMaskParamBit, p};
            e.path = "mask." + (mask.name.empty() ? "?" : mask.name) + "." +
                     kMaskParamIds[p];
            e.label = "mask " + mask.name + " " + kMaskParamIds[p];
            mask_param_range(p, &e.min_value, &e.max_value);
            e.base = *slot;
            table.push_back(std::move(e));
        }
        // Bezier path points (spec §8 "keyframable points"): every
        // coordinate is addressable past kMaskPointParamBase.
        for (size_t pt = 0; pt * 2 + 1 < mask.points.size(); ++pt) {
            for (int a = 0; a < 2; ++a) {
                const int p = doc::kMaskPointParamBase +
                              static_cast<int>(pt) * 2 + a;
                ParamEntry e;
                e.key = {mask.id | doc::kMaskParamBit, p};
                e.path = "mask." + (mask.name.empty() ? "?" : mask.name) +
                         ".p" + std::to_string(pt) + (a ? ".y" : ".x");
                e.label = "mask " + mask.name + " point " +
                          std::to_string(pt) + (a ? " y" : " x");
                mask_param_range(p, &e.min_value, &e.max_value);
                e.base = mask.points[pt * 2 + static_cast<size_t>(a)];
                table.push_back(std::move(e));
            }
        }
    }
    return table;
}

}  // namespace looks::mod
