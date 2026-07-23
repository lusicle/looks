// Undoable layer mutations (spec §10). Layer property edits are whole-
// object replacements minus the stack; coalesces per layer so opacity
// drags are one undo step.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Fresh layer with a unique id and a default name for its kind.
Layer make_layer(Document& doc, LayerSourceKind kind);

std::unique_ptr<Command> add_layer_command(Layer layer, size_t insert_index);
// Refuses nothing here — the app enforces "at least one layer" and cleans
// up mod routes/lanes targeting removed effects lazily (dangling ids are
// skipped at eval and restored by undo).
std::unique_ptr<Command> remove_layer_command(size_t layer_index);
// Everything but the stack; matched by layer id, coalesces per layer.
std::unique_ptr<Command> set_layer_props_command(Layer updated);
// Swaps a layer with a neighbour (compositing reorder). `direction` is
// -1 (toward the bottom of the stack) or +1; caller bounds-checks.
std::unique_ptr<Command> move_layer_command(size_t index, int direction);
// Whole-layer replacement including the stack — resets the last remaining
// layer, since removing it outright would leave the document empty.
std::unique_ptr<Command> replace_layer_command(size_t index, Layer fresh);

}  // namespace looks::doc
