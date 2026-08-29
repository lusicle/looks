// The intensity is 0 to 1. The caller must advance rng_seed each time.

#pragma once

#include "doc/command.h"
#include "doc/document.h"
#include "doc/effects.h"

namespace looks::doc {

// A selector param holds the identity of the effect: it does not change.
bool param_randomizable(const ParamDesc& desc);

void randomize_effect(Document& doc, UndoStack& undo, uint64_t look,
                      size_t layer_index, size_t effect_index,
                      float intensity, uint64_t rng_seed);
void randomize_stack(Document& doc, UndoStack& undo, uint64_t look,
                     size_t layer_index, float intensity, uint64_t rng_seed);

}  // namespace looks::doc
