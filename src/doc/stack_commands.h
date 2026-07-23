// Undoable mutations of a layer's effect stack (spec §10: every mutation is
// a Command). Param drags execute with coalesce=true — SetParamCommand
// merges consecutive edits of the same knob so a whole gesture is one undo
// step; the app calls break_coalescing() on mouse-up.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// param_index addresses EffectInstance::params; the sentinels edit the
// built-in wet/dry and opacity knobs.
inline constexpr int kWetParam = -1;
inline constexpr int kOpacityParam = -2;

std::unique_ptr<Command> set_param_command(size_t layer_index,
                                           size_t effect_index,
                                           int param_index, float new_value);
std::unique_ptr<Command> set_bypass_command(size_t layer_index,
                                            size_t effect_index, bool bypass);
// Solo (spec §5): any soloed effect mutes the rest of its stack.
std::unique_ptr<Command> set_solo_command(size_t layer_index,
                                          size_t effect_index, bool solo);
// Takes the fully-formed instance (id already assigned via make_effect) so
// redo re-inserts the identical object.
std::unique_ptr<Command> add_effect_command(size_t layer_index,
                                            EffectInstance instance,
                                            size_t insert_index);
std::unique_ptr<Command> remove_effect_command(size_t layer_index,
                                               size_t effect_index);
std::unique_ptr<Command> move_effect_command(size_t layer_index,
                                             size_t from_index,
                                             size_t to_index);

}  // namespace looks::doc
