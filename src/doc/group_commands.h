// Undoable group mutations (the Group node collapses a
// sub-stack and exposes member params on its face as direct aliases).
// Membership is EffectInstance::group_id; these commands keep that tag
// and the Layer::groups list consistent.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Fresh group with a minted id (shares the effect id space — group ids and
// effect ids never collide, which keeps preset remapping simple).
Group make_group(Document& doc, std::string name);

// Wrap the contiguous stack range [from, to] (inclusive) into `group`.
// Effects already in another group are re-tagged.
std::unique_ptr<Command> group_effects_command(size_t layer_index, Group group,
                                               size_t from, size_t to);

// Dissolve the group: members keep their stack slots, group_id clears.
std::unique_ptr<Command> ungroup_command(size_t layer_index,
                                         uint64_t group_id);

// Re-tag one effect's membership (join a group with group_id, or leave
// with 0). Callers dissolve groups that end up empty.
std::unique_ptr<Command> set_effect_group_command(size_t layer_index,
                                                  size_t effect_index,
                                                  uint64_t group_id);

// Name / folded / bypass / exposed face, matched by group id. Merges per
// group so repeated edits coalesce into one undo step.
std::unique_ptr<Command> set_group_props_command(size_t layer_index,
                                                 Group updated);

// Toggle one member param on/off the group face (texed expose).
std::unique_ptr<Command> set_group_exposed_command(size_t layer_index,
                                                   uint64_t group_id,
                                                   ParamKey key,
                                                   bool exposed);

// Append a whole group (its member effects + the Group entry) to a layer's
// stack — the preset-instantiation path. Effects must already carry
// group.id in their group_id and fresh document ids.
std::unique_ptr<Command> insert_group_command(size_t layer_index, Group group,
                                              std::vector<EffectInstance> effects);

}  // namespace looks::doc
