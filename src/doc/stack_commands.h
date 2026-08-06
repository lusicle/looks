// Undoable mutations of a layer's effect stack (every mutation is
// a Command). Param drags execute with coalesce=true — SetParamCommand
// merges consecutive edits of the same knob so a whole gesture is one undo
// step; the app calls break_coalescing() on mouse-up.
//
// Every command names the look it edits: undo must land where the edit was
// made, not wherever the UI is scoped when it runs.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// param_index addresses EffectInstance::params; the sentinels edit the
// built-in wet/dry and opacity knobs.
inline constexpr int kWetParam = -1;
inline constexpr int kOpacityParam = -2;

std::unique_ptr<Command> set_param_command(uint64_t look, size_t layer_index,
                                           size_t effect_index,
                                           int param_index, float new_value);
std::unique_ptr<Command> set_bypass_command(uint64_t look, size_t layer_index,
                                            size_t effect_index, bool bypass);
// The Text effect's string — the one non-float param.
std::unique_ptr<Command> set_effect_text_command(uint64_t look,
                                                 size_t layer_index,
                                                 size_t effect_index,
                                                 std::string text);
// Solo: any soloed effect mutes the rest of its stack.
std::unique_ptr<Command> set_solo_command(uint64_t look, size_t layer_index,
                                          size_t effect_index, bool solo);
// Takes the fully-formed instance (id already assigned via make_effect) so
// redo re-inserts the identical object.
std::unique_ptr<Command> add_effect_command(uint64_t look, size_t layer_index,
                                            EffectInstance instance,
                                            size_t insert_index);
std::unique_ptr<Command> remove_effect_command(uint64_t look,
                                               size_t layer_index,
                                               size_t effect_index);
std::unique_ptr<Command> move_effect_command(uint64_t look, size_t layer_index,
                                             size_t from_index,
                                             size_t to_index);

// Node-canvas placement (docs/flow_canvas.md): one command moves any node
// kind, addressed by document id. Consecutive moves of the same node
// coalesce so a whole drag is one undo step. Positions are pure UI state
// on the document — the renderer never reads them.
enum class NodeRef : uint32_t {
    Effect, Layer, Route, Output, Frame, Group,
    GroupIn, GroupOut,   // a group's boundary nodes (id = the group)
};
std::unique_ptr<Command> set_node_pos_command(uint64_t look, NodeRef kind,
                                              uint64_t id, float x, float y);

// TRUE GRAPH link edits (docs/flow_canvas.md). Both materialize the
// synthesized legacy links on first edit, so the table becomes the single
// topology truth from then on. connect replaces any existing link at
// (to, port) — except the Output node (to 0), which accepts any number of
// composite inputs. Callers validate with link_would_cycle FIRST; the
// commands themselves apply unconditionally.
std::unique_ptr<Command> connect_command(uint64_t look, NodeLink link);
std::unique_ptr<Command> disconnect_command(uint64_t look, NodeLink link);
// Freeze the synthesized legacy wiring: run before ANY node add so
// the newborn spawns unwired instead of being chained in by stack-order
// synthesis. No-op when links are already materialized (or no layers).
std::unique_ptr<Command> materialize_links_command(uint64_t look);

// True when adding from→to would close a cycle: to already reaches from
// through the (effective) link table. The texed reachability guard.
bool link_would_cycle(const Look& look, uint64_t from, uint64_t to);

// Canvas frames (docs/flow_canvas.md): titled grouping boxes.
std::unique_ptr<Command> add_frame_command(uint64_t look, CanvasFrame frame);
std::unique_ptr<Command> remove_frame_command(uint64_t look,
                                              uint64_t frame_id);
// Resize coalesces per frame id (corner drag = one undo step).
std::unique_ptr<Command> set_frame_bounds_command(uint64_t look,
                                                  uint64_t frame_id, float w,
                                                  float h);
std::unique_ptr<Command> set_frame_title_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 std::string title);
// Colour tag cycle (0 = none, 1..8 = palette hue).
std::unique_ptr<Command> set_frame_color_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 uint32_t color);

}  // namespace looks::doc
