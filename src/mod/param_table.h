// Global addressable param table: every numeric param of the
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

// True for integer-semantics params (selectors and flagged counts) —
// resolve snaps their modulated value to whole numbers.
bool param_discrete(doc::EffectType type, int param_index);

// Current value of a param on an instance (wet/opacity aware).
float param_value(const doc::EffectInstance& fx, int param_index);

// Layer params as mod targets (kLayerParamBit): opacity, generator
// fields, transform. Null for out-of-range indices.
float* layer_param_slot(doc::Layer& layer, int param_index);
void layer_param_range(int param_index, float* min_value, float* max_value);

}  // namespace looks::mod
