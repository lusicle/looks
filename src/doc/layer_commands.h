// Undoable layer mutations. Layer property edits are whole-
// object replacements minus the stack; coalesces per layer so opacity
// drags are one undo step. Every command names the look it edits.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Fresh layer with a unique id and a default name for its kind.
Layer make_layer(Document& doc, LayerSourceKind kind);

std::unique_ptr<Command> add_layer_command(uint64_t look, Layer layer,
                                           size_t insert_index);
// Refuses nothing here — the app enforces "at least one layer" and cleans
// up mod routes/lanes targeting removed effects lazily (dangling ids are
// skipped at eval and restored by undo).
std::unique_ptr<Command> remove_layer_command(uint64_t look,
                                              size_t layer_index);
// Everything but the stack; matched by layer id, coalesces per layer.
std::unique_ptr<Command> set_layer_props_command(uint64_t look, Layer updated);
// Razor: splits a placement at a LOCAL frame into two abutting
// placements on the SAME lane. The right half is a fresh placement of
// the same target whose source_in lands on the cut's source frame, so
// every frame renders exactly as before the cut. Nothing else moves: no
// clone, no wiring - sequences own no effects, so razor identity is
// structural. Cutting never forks the target. Null when `at` does not
// fall strictly inside a placement, and at the per-lane placement
// bound. Takes the document to mint the right half's id at build time.
std::unique_ptr<Command> razor_track_command(Document& doc,
                                             uint64_t sequence,
                                             uint64_t track_id, uint32_t at);
// The same cut for a block on an AUDIO track (an unlinked sound edits on
// its own; a linked one splits its whole group either way).
std::unique_ptr<Command> razor_audio_command(Document& doc,
                                             uint64_t sequence,
                                             uint64_t track_id, uint32_t at);

}  // namespace looks::doc
