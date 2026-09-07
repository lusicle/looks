#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

Layer make_layer(Document& doc, LayerSourceKind kind);

std::unique_ptr<Command> add_layer_command(uint64_t look, Layer layer,
                                           size_t insert_index);
std::unique_ptr<Command> remove_layer_command(uint64_t look,
                                              size_t layer_index);
// Replaces all layer fields but the stack. Coalesces per layer id.
std::unique_ptr<Command> set_layer_props_command(uint64_t look, Layer updated);
// Returns null if `at` is not strictly inside a placement, `at` is local.
// The cut applies to the whole link group, thus the halves stay in sync.
std::unique_ptr<Command> razor_track_command(Document& doc,
                                             uint64_t sequence,
                                             uint64_t track_id, uint32_t at);
std::unique_ptr<Command> razor_audio_command(Document& doc,
                                             uint64_t sequence,
                                             uint64_t track_id, uint32_t at);

}  // namespace looks::doc
