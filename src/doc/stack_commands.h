// Param edits coalesce per knob. The app calls break_coalescing on mouse-up.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// param_index addresses EffectInstance::params. These ids are built-ins.
inline constexpr int kWetParam = -1;
inline constexpr int kOpacityParam = -2;

std::unique_ptr<Command> set_param_command(uint64_t look, size_t layer_index,
                                           size_t effect_index,
                                           int param_index, float new_value);
// Base writes and lane writes to one effect land in one undo step.
// A lane write replaces the keys only. Loop and mute stay as they are.
struct ParamWrite {
    int param_index;
    float value;
};
std::unique_ptr<Command> set_param_gesture_command(
    uint64_t look, size_t layer_index, size_t effect_index,
    std::vector<ParamWrite> base_writes,
    std::vector<KeyframeLane> lane_writes);
std::unique_ptr<Command> set_bypass_command(uint64_t look, size_t layer_index,
                                            size_t effect_index, bool bypass);
std::unique_ptr<Command> set_effect_blend_command(uint64_t look,
                                                  size_t layer_index,
                                                  size_t effect_index,
                                                  BlendMode blend);
std::unique_ptr<Command> set_effect_text_command(uint64_t look,
                                                 size_t layer_index,
                                                 size_t effect_index,
                                                 std::string text);
// A soloed effect mutes the other effects in its stack.
std::unique_ptr<Command> set_solo_command(uint64_t look, size_t layer_index,
                                          size_t effect_index, bool solo);
// The instance must have its id already, from make_effect.
std::unique_ptr<Command> add_effect_command(uint64_t look, size_t layer_index,
                                            EffectInstance instance,
                                            size_t insert_index);
std::unique_ptr<Command> remove_effect_command(uint64_t look,
                                               size_t layer_index,
                                               size_t effect_index);
std::unique_ptr<Command> move_effect_command(uint64_t look, size_t layer_index,
                                             size_t from_index,
                                             size_t to_index);

// Node positions are UI state. The renderer does not read them.
// Moves of the same node coalesce into one undo step.
enum class NodeRef : uint32_t {
    Effect, Layer, Route, Output, Frame, Group,
    GroupIn, GroupOut,   // boundary nodes: the id is the group id
};
std::unique_ptr<Command> set_node_pos_command(uint64_t look, NodeRef kind,
                                              uint64_t id, float x, float y);

// An empty link table means implicit stack-order wiring. An edit freezes it.
// The caller must check link_would_cycle first: these apply unconditionally.
std::unique_ptr<Command> connect_command(uint64_t look, NodeLink link);
std::unique_ptr<Command> disconnect_command(uint64_t look, NodeLink link);
// new_link takes the position of old_link, thus stacking order stays.
std::unique_ptr<Command> reconnect_command(uint64_t look, NodeLink old_link,
                                           NodeLink new_link);
// Link order in a port fan-in is stacking order. This swaps two feeds.
std::unique_ptr<Command> move_port_link_command(uint64_t look, uint64_t to,
                                                uint32_t to_port,
                                                size_t index, int delta);
// Run this before any node add, or stack-order wiring chains the new node.
std::unique_ptr<Command> materialize_links_command(uint64_t look);

bool link_would_cycle(const Look& look, uint64_t from, uint64_t to);

std::unique_ptr<Command> add_frame_command(uint64_t look, CanvasFrame frame);
std::unique_ptr<Command> remove_frame_command(uint64_t look,
                                              uint64_t frame_id);
// This coalesces per frame id, thus a resize drag is one undo step.
std::unique_ptr<Command> set_frame_bounds_command(uint64_t look,
                                                  uint64_t frame_id, float w,
                                                  float h);
std::unique_ptr<Command> set_frame_title_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 std::string title);
// A color of 0 is none. Values 1 to 8 are palette hues.
std::unique_ptr<Command> set_frame_color_command(uint64_t look,
                                                 uint64_t frame_id,
                                                 uint32_t color);

}  // namespace looks::doc
