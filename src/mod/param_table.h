// ParamKey is the stable address. The path is for display only.

#pragma once

#include <string>
#include <vector>

#include "doc/document.h"

namespace looks::mod {

struct ParamEntry {
    doc::ParamKey key;
    std::string path;
    std::string label;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float base = 0.0f;       // The current value in the document.
};

std::vector<ParamEntry> build_param_table(const doc::Document& doc,
                                          const doc::Look& look);

// A route can dangle. The doc keeps it so that undo can restore it.
const doc::EffectInstance* find_effect(const doc::Look& look,
                                       uint64_t effect_id,
                                       size_t* index_out = nullptr);

// Effect ids are unique across looks.
const doc::EffectInstance* find_effect(const doc::Document& doc,
                                       uint64_t effect_id,
                                       uint64_t* look_out,
                                       size_t* index_out = nullptr);

int source_param_index_of(const std::string& id);

void param_range(doc::EffectType type, int param_index, float* min_value,
                 float* max_value);

bool param_discrete(doc::EffectType type, int param_index);

float param_value(const doc::EffectInstance& fx, int param_index);

// Returns null for out-of-range indices.
float* source_param_slot(doc::Source& layer, int param_index);
void source_param_range(int param_index, float* min_value, float* max_value);

}  // namespace looks::mod
