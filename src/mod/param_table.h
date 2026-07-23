// Global addressable param table (spec §5): every numeric param of the
// document enumerated with a string path ("layer0.fx2.shift_x") plus range
// metadata. Rebuilt from the document on demand (it's tiny); modulation
// targets address params by stable ParamKey — the path is display/serialize
// sugar derived from the current stack order. MIDI/OSC bind onto these
// paths later for free.

#pragma once

#include <string>
#include <vector>

#include "doc/document.h"

namespace looks::mod {

struct ParamEntry {
    doc::ParamKey key;
    std::string path;        // "layer0.fx<pos>.<param_id>"
    std::string label;       // "<Effect> <param label>"
    float min_value = 0.0f;
    float max_value = 1.0f;
    float base = 0.0f;       // current document value
};

std::vector<ParamEntry> build_param_table(const doc::Document& doc);

// Effect lookup by stable id across all layers; returns null when the
// effect no longer exists (dangling routes/lanes are skipped at eval, kept
// in the doc so undo can resurrect their target).
const doc::EffectInstance* find_effect(const doc::Document& doc,
                                       uint64_t effect_id,
                                       size_t* layer_out = nullptr,
                                       size_t* index_out = nullptr);

// Range of a param addressed by (type, param_index) incl. wet/opacity.
void param_range(doc::EffectType type, int param_index, float* min_value,
                 float* max_value);

// Current value of a param on an instance (wet/opacity aware).
float param_value(const doc::EffectInstance& fx, int param_index);

// Mask params as mod targets (spec §8). Slot for a mask param index (see
// kMaskParamBit in modulation.h); null for out-of-range indices.
float* mask_param_slot(doc::Mask& mask, int param_index);
void mask_param_range(int param_index, float* min_value, float* max_value);

}  // namespace looks::mod
