#pragma once

#include <cstdint>

#include "doc/document.h"

namespace looks::doc {

struct ParamDesc {
    const char* id;
    const char* label;
    float min_value;
    float max_value;
    float default_value;
    const char* format;    // printf-style readout
    // '|'-separated options in value order from min_value. null = a slider.
    const char* options = nullptr;
    // true snaps the modulated value to whole numbers. options imply it.
    bool integer = false;
    // vis_param names a sibling selector; bit v of vis_mask shows this row
    // at value v. -1 = always. Kernels must ignore params their mode hides.
    int8_t vis_param = -1;
    uint32_t vis_mask = 0;
    // The value is in radians; the row shows degrees.
    bool display_deg = false;
    float hue_bar = -1.0f;
};

inline bool param_is_hue(const ParamDesc& d) { return d.hue_bar >= 0.0f; }

inline bool param_visible(const EffectInstance& fx, const ParamDesc& d) {
    if (d.vis_param < 0) return true;
    const size_t ctrl = static_cast<size_t>(d.vis_param);
    if (ctrl >= fx.params.size()) return true;
    const int v = static_cast<int>(fx.params[ctrl] + 0.5f);
    return v >= 0 && v < 32 && ((d.vis_mask >> v) & 1u) != 0;
}

inline bool param_is_discrete(const ParamDesc& d) {
    return d.integer || d.options != nullptr;
}

inline int param_option_count(const char* options) {
    if (!options || !*options) return 0;
    int n = 1;
    for (const char* c = options; *c; ++c)
        if (*c == '|') ++n;
    return n;
}

inline const char* param_option_at(const char* options, int index,
                                   int* length) {
    const char* start = options;
    const char* c = options;
    int i = 0;
    for (;; ++c) {
        if (*c == '|' || *c == '\0') {
            if (i == index || *c == '\0') {
                *length = static_cast<int>(c - start);
                return start;
            }
            ++i;
            start = c + 1;
        }
    }
}

// Enum order is the menu order. It is not serialized.
enum class FxCategory : uint8_t {
    Time = 0,
    Warp,
    Optics,
    Color,
    Texture,
    Mosaic,
    PaintPrint,
    Signal,
    Overlay,
    Audio,
    Count,
};

const char* fx_category_label(FxCategory category);

struct EffectInfo {
    const char* id;        // stable string id
    const char* label;
    const ParamDesc* params;
    uint32_t param_count;
    FxCategory category = FxCategory::Signal;
};

const EffectInfo& effect_info(EffectType type);

EffectInstance make_effect(Document& doc, EffectType type);

// These effects are not pure per frame: the render cache must skip them.
bool effect_uses_history(EffectType type);

bool look_uses_history(const Look& look);
bool document_uses_history(const Document& doc);

}  // namespace looks::doc
