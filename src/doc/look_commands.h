// Undoable mutations of the project's looks, sequences and assets.
// A look is a timeless node graph; a sequence is arrangement; an asset is
// imported media that clip nodes bind to by id.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Fresh empty look with a minted id (shares the effect id space, so a
// look id never collides with a node id).
Look make_look(Document& doc, std::string name);
// Fresh empty sequence with one video lane, same id space.
Sequence make_sequence(Document& doc, std::string name);
// Fresh asset entry for an imported bundle.
Asset make_asset(Document& doc, std::string name, std::string path);

std::unique_ptr<Command> add_look_command(Look look);
// Sources referencing a removed look go dormant, the way routes
// targeting a removed effect do; undo restores both.
std::unique_ptr<Command> remove_look_command(uint64_t look_id);
// Name + explicit duration (0 = derive from the longest source).
// Coalesces per look so a duration drag is one undo step.
std::unique_ptr<Command> set_look_props_command(uint64_t look,
                                                std::string name,
                                                uint32_t duration);

std::unique_ptr<Command> add_sequence_command(Sequence seq);
// Blocks and sources referencing a removed sequence go dormant; undo
// restores them. Callers never name the root sequence (the project
// timeline) - the UI does not offer it.
std::unique_ptr<Command> remove_sequence_command(uint64_t sequence_id);
// Name + explicit duration (0 = derive from the furthest block end).
std::unique_ptr<Command> set_sequence_props_command(uint64_t sequence,
                                                    std::string name,
                                                    uint32_t duration);

std::unique_ptr<Command> add_asset_command(Asset asset);
// Whole-asset replacement matched by id: rebinding a path, or writing
// back what opening the bundle probed (frame count, fps, still length).
std::unique_ptr<Command> set_asset_command(Asset updated);

// Browser bins: project-panel folders. Membership is the `bin` field on
// looks, sequences and assets; bins nest by parent.
Bin make_bin(Document& doc, std::string name);
std::unique_ptr<Command> add_bin_command(Bin bin);
// Deleting a bin keeps its contents: members and child bins move up to
// the bin's parent; undo restores every membership and the bin itself.
std::unique_ptr<Command> remove_bin_command(uint64_t bin_id);
// Rename + reparent in one. Callers guard cycles with bin_reaches FIRST
// (a bin must not land inside its own subtree).
std::unique_ptr<Command> set_bin_props_command(uint64_t bin,
                                               std::string name,
                                               uint64_t parent);
// Files a look, sequence or asset under `bin` (0 = project root).
std::unique_ptr<Command> set_entity_bin_command(uint64_t entity_id,
                                                uint64_t bin);

// NEST: moves `layer_ids` out of `look` into a brand new look and puts a
// LookRef source node in their place; Ctrl+G one level up. Everything
// plays in lockstep, so nothing is rebased - the nested look renders
// exactly what the members rendered.
//
// Wiring: links with BOTH ends inside the selection travel with it, and
// so do its feeds to the Output (they become the new look's Output). A
// link crossing the boundary is DROPPED - the same break grouping makes -
// because the nested look has no port to carry it. Undo restores every
// dropped link.
//
// Null when the selection is empty, names nothing that exists, or would
// leave the parent look with nothing.
std::unique_ptr<Command> nest_layers_command(
    Document& doc, uint64_t look, const std::vector<uint64_t>& layer_ids,
    std::string name);

// MAKE UNIQUE: sharing is the default, so this explicit fork must exist
// or the default is a trap. Deep-copies the look or sequence the
// placement targets - every id reminted, wiring and mod targets
// remapped; nested references inside the copy keep pointing at the SAME
// shared entities (the fork is one level deep, like the edit that wants
// it) - and retargets THAT placement at the fork; every other placement
// keeps the original. Null when the placement does not target a live
// entity, or at the entity-count bound.
std::unique_ptr<Command> make_unique_command(Document& doc,
                                             uint64_t sequence,
                                             uint64_t placement_id);

}  // namespace looks::doc
