
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Group ids come from the effect id space. They never collide.
Group make_group(Document& doc, std::string name);

std::unique_ptr<Command> group_effects_command(uint64_t look,
                                               Group group,
                                               std::vector<uint64_t> members);

std::unique_ptr<Command> ungroup_command(uint64_t look, uint64_t group_id);

// A group_id of 0 leaves the group. The caller dissolves an empty group.
std::unique_ptr<Command> set_effect_group_command(uint64_t look,
                                                  uint64_t effect_id,
                                                  uint64_t group_id);

// Whole-group replacement by id. It coalesces per group id.
std::unique_ptr<Command> set_group_props_command(uint64_t look,
                                                 Group updated);

std::unique_ptr<Command> set_group_exposed_command(uint64_t look,
                                                   uint64_t group_id,
                                                   ParamKey key,
                                                   bool exposed);

// The effects must carry group.id in group_id and have fresh document ids.
std::unique_ptr<Command> insert_group_command(uint64_t look,
                                              Group group,
                                              std::vector<EffectInstance> effects,
                                              std::vector<NodeLink> links);

// The caller mints slot_id. The slot arrives unwired.
std::unique_ptr<Command> add_group_input_command(uint64_t look,
                                                 uint64_t group_id,
                                                 uint64_t slot_id);

// This also removes every link that touches the slot.
std::unique_ptr<Command> remove_group_input_command(uint64_t look,
                                                    uint64_t group_id,
                                                    uint64_t slot_id);

// This is idempotent: wiring that has slots has no crossings left.
// A seed_member of 0 skips the interior In slot at inputs[0].
void normalize_group_inputs(Document& doc, Look& look, Group& g, uint64_t seed_member);

}  // namespace looks::doc
