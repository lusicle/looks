// Undoable group mutations (the Group node collapses a
// sub-stack and exposes member params on its face as direct aliases).
// Membership is EffectInstance::group_id; these commands keep that tag
// and the Layer::groups list consistent. Every command names its look.

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
std::unique_ptr<Command> group_effects_command(uint64_t look,
                                               size_t layer_index, Group group,
                                               size_t from, size_t to);

// Dissolve the group: members keep their stack slots, group_id clears.
std::unique_ptr<Command> ungroup_command(uint64_t look, size_t layer_index,
                                         uint64_t group_id);

// Re-tag one effect's membership (join a group with group_id, or leave
// with 0). Callers dissolve groups that end up empty.
std::unique_ptr<Command> set_effect_group_command(uint64_t look,
                                                  size_t layer_index,
                                                  size_t effect_index,
                                                  uint64_t group_id);

// Name / folded / bypass / exposed face, matched by group id. Merges per
// group so repeated edits coalesce into one undo step.
std::unique_ptr<Command> set_group_props_command(uint64_t look,
                                                 size_t layer_index,
                                                 Group updated);

// Toggle one member param on/off the group face (texed expose).
std::unique_ptr<Command> set_group_exposed_command(uint64_t look,
                                                   size_t layer_index,
                                                   uint64_t group_id,
                                                   ParamKey key,
                                                   bool exposed);

// Append a whole group (its member effects + the Group entry) to a layer's
// stack — the preset-instantiation path. Effects must already carry
// group.id in their group_id and fresh document ids. `face_in` names the
// member the seeded In slot wires to (0 = first member).
std::unique_ptr<Command> insert_group_command(uint64_t look,
                                              size_t layer_index, Group group,
                                              std::vector<EffectInstance> effects,
                                              uint64_t face_in);

// Append one input slot to a group (id pre-minted from next_effect_id).
// The slot arrives unwired; the caller's connect lands the exterior
// wire, interior wiring is the user's gesture in the open view.
std::unique_ptr<Command> add_group_input_command(uint64_t look,
                                                 size_t layer_index,
                                                 uint64_t group_id,
                                                 uint64_t slot_id);

// Remove one input slot and every link touching it (both sides).
std::unique_ptr<Command> remove_group_input_command(uint64_t look,
                                                    size_t layer_index,
                                                    uint64_t group_id,
                                                    uint64_t slot_id);

// Gives a group its input slots: every crossing link (outside producer
// into a member port) reroutes through a slot - the slot takes the
// crossing's position in the member port's fan-in, the producers append
// as the slot's own fan-in in their old order - and when no slot ends up
// wired to a member's port 0, `seed_member` (0 = skip) gets an
// interior-only In slot at inputs[0]. Idempotent: slotted wiring has no
// crossings left. Mints ids from next_effect_id; materializes
// synthesized links first when it changes anything. Shared by group
// creation and the loader's legacy migration.
void normalize_group_inputs(Document& doc, Look& look, size_t layer_index,
                            Group& g, uint64_t seed_member);

}  // namespace looks::doc
