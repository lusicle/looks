// Effect metadata: a static per-type table of param descriptors —
// the inspector, randomizer, and the future global param table all read one
// source of truth. The instance type itself lives in document.h.

#pragma once

#include <cstdint>

#include "doc/document.h"

namespace looks::doc {

struct ParamDesc {
    const char* id;        // path segment: "layer0.fx2.<id>"
    const char* label;     // inspector display
    float min_value;
    float max_value;
    float default_value;
    const char* format;    // slider readout, printf-style
    // Discrete selector options, '|'-separated ("luma|bright key|...") in
    // value order from min_value — non-null turns the row into a DROPDOWN
    // on the canvas card and in the inspector (real controls).
    // Continuous params leave it null and keep the slider.
    const char* options = nullptr;
    // True integer semantics (counts, indices, ring depths): resolve snaps
    // the modulated value to whole numbers so keyframe/route/morph
    // interpolation never feeds kernels fractional counts (a
    // fractional dither `levels` puts a hard band through the frame).
    // Selector params are integer implicitly via `options`; %.0f params
    // where fractions are meaningful (px radii, hz rates) stay false.
    bool integer = false;
};

inline bool param_is_discrete(const ParamDesc& d) {
    return d.integer || d.options != nullptr;
}

// '|'-separated option helpers: count, and the start/length of
// entry `index` (clamped). Header-inline so the canvas and the rail share
// one parse.
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

// effect families — drives the grouped add-effect browser.
// Enum order IS the menu order; not serialized, safe to restructure.
enum class FxCategory : uint8_t {
    Time = 0,     // time & motion
    Warp,         // warp & displace
    Optics,       // optics & light
    Color,        // color & tone
    Texture,      // texture & detail
    Mosaic,       // structure & scan (line render, sims, scan synths)
    PaintPrint,   // paint & print (painterly + repro processes)
    Signal,       // signal & codec
    Overlay,      // frame & overlay
    Audio,        // audio & dsp (voice modifiers - image passes through)
    Count,
};

const char* fx_category_label(FxCategory category);

struct EffectInfo {
    const char* id;        // stable string id ("rgb_split")
    const char* label;     // display name ("RGB Split")
    const ParamDesc* params;
    uint32_t param_count;
    FxCategory category = FxCategory::Signal;
};

const EffectInfo& effect_info(EffectType type);

// Fresh instance with defaults; takes its stable id from doc.next_effect_id.
EffectInstance make_effect(Document& doc, EffectType type);

// True for effects whose output depends on render history — persistent
// engine state (feedback, slit ring, Codec-Box decoder, RD sim, temporal
// error carry) or the previous frame's luma (flow consumers). Their frames
// are not pure functions of (document, frame index), so the frame render
// cache must not serve them.
bool effect_uses_history(EffectType type);

// Scans every layer stack (bypassed effects excluded: they never
// dispatch). True disables the render cache for the document.
bool look_uses_history(const Look& look);
bool document_uses_history(const Document& doc);

}  // namespace looks::doc
