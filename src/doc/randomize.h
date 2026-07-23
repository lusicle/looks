// Randomize / mutate (spec §10): per-effect or whole stack, intensity 0..1,
// respecting param ranges and per-param randomizable-ness. One undo step
// per gesture. Deterministic per rng_seed — callers advance a counter.

#pragma once

#include "doc/command.h"
#include "doc/document.h"
#include "doc/effects.h"

namespace looks::doc {

// Selectors (modes, palettes, glyph sets, corners) hold the LOOK's identity
// and stay put; continuous params are fair game.
bool param_randomizable(const ParamDesc& desc);

void randomize_effect(Document& doc, UndoStack& undo, size_t layer_index,
                      size_t effect_index, float intensity, uint64_t rng_seed);
void randomize_stack(Document& doc, UndoStack& undo, size_t layer_index,
                     float intensity, uint64_t rng_seed);

}  // namespace looks::doc
