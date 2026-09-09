#pragma once

#include <memory>
#include <string>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Look and sequence ids come from the effect id space. They never collide.
Look make_look(Document& doc, std::string name);
Sequence make_sequence(Document& doc, std::string name);
Asset make_asset(Document& doc, std::string name, std::string path);

std::unique_ptr<Command> add_look_command(Look look);
// Sources that name a removed look go dormant. Undo restores them.
std::unique_ptr<Command> remove_look_command(uint64_t look_id);
// A duration of 0 makes the look derive it from the longest source.
// This command coalesces per look, thus a drag is one undo step.
std::unique_ptr<Command> set_look_props_command(uint64_t look,
                                                std::string name,
                                                uint32_t duration);
// split false = the voice rides the In wire chain. split true = port 1.
std::unique_ptr<Command> set_look_audio_split_command(uint64_t look,
                                                      bool split);

std::unique_ptr<Command> add_sequence_command(Sequence seq);
// Blocks and sources that name a removed sequence go dormant.
// The UI does not offer the root sequence, thus this does not refuse it.
std::unique_ptr<Command> remove_sequence_command(uint64_t sequence_id);
// A duration of 0 makes the sequence derive it from the last block end.
std::unique_ptr<Command> set_sequence_props_command(uint64_t sequence,
                                                    std::string name,
                                                    uint32_t duration);

std::unique_ptr<Command> add_asset_command(Asset asset);
// Whole-asset replacement matched by id: copy it, then change fields.
std::unique_ptr<Command> set_asset_command(Asset updated);
// Media layers that name the id go dormant. The file stays on disk.
std::unique_ptr<Command> remove_asset_command(uint64_t asset_id);

// Bin membership is the `bin` field on looks, sequences and assets.
Bin make_bin(Document& doc, std::string name);
std::unique_ptr<Command> add_bin_command(Bin bin);
// Members and child bins move up to the parent bin. They stay in the doc.
std::unique_ptr<Command> remove_bin_command(uint64_t bin_id);
// The caller must guard cycles with bin_reaches first.
std::unique_ptr<Command> set_bin_props_command(uint64_t bin,
                                               std::string name,
                                               uint64_t parent);
// A bin of 0 is the project root.
std::unique_ptr<Command> set_entity_bin_command(uint64_t entity_id,
                                                uint64_t bin);

// The fork is one level deep: nested references in the copy stay shared.
std::unique_ptr<Command> make_unique_command(Document& doc,
                                             uint64_t sequence,
                                             uint64_t placement_id);

}  // namespace looks::doc
