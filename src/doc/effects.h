// Effect metadata (spec §5): a static per-type table of param descriptors —
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
};

// Spec §6.1 effect families — drives the grouped add-effect browser.
enum class FxCategory : uint8_t {
    Time = 0,     // time & motion
    Warp,         // warp & displace
    Optics,       // optics & light
    Color,        // color & tone
    Texture,      // texture & detail
    Mosaic,       // mosaic & structure
    Signal,       // signal & codec
    Overlay,      // frame & overlay
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
// cache must not serve them (spec §10 vs §11).
bool effect_uses_history(EffectType type);

// Scans every layer stack and mask mini-chain (bypassed effects excluded:
// they never dispatch). True disables the render cache for the document.
bool document_uses_history(const Document& doc);

}  // namespace looks::doc
