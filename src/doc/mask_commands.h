// Undoable mask mutations (spec §10). Mask param edits are whole-object
// replacements minus the chain (masks are small); the mini chain reuses the
// generic pattern from stack commands via chain-specific commands.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

Mask* find_mask(Document& doc, uint64_t mask_id);
const Mask* find_mask(const Document& doc, uint64_t mask_id);

std::unique_ptr<Command> add_mask_command(Mask mask);
// Also clears mask_id on any effect referencing it (restored on undo).
std::unique_ptr<Command> remove_mask_command(uint64_t mask_id);
// Replaces every field EXCEPT the chain; coalesces per mask (slider drags).
std::unique_ptr<Command> set_mask_params_command(Mask updated);
// Removes point pair `point_index` from a shape mask's bezier path and
// keeps point keyframe lanes consistent: the removed point's lanes die,
// lanes of higher-indexed points shift down one pair (undo restores both).
std::unique_ptr<Command> remove_mask_point_command(uint64_t mask_id,
                                                   size_t point_index);
// Assigns a mask (or 0) to a stack effect.
std::unique_ptr<Command> set_effect_mask_command(size_t layer_index,
                                                 size_t effect_index,
                                                 uint64_t mask_id);
// Mini-chain edits.
std::unique_ptr<Command> mask_chain_add_command(uint64_t mask_id,
                                                EffectInstance instance);
std::unique_ptr<Command> mask_chain_remove_command(uint64_t mask_id,
                                                   size_t chain_index);
// Coalesces per (mask, chain_index, param).
std::unique_ptr<Command> mask_chain_set_param_command(uint64_t mask_id,
                                                      size_t chain_index,
                                                      int param_index,
                                                      float new_value);

}  // namespace looks::doc
