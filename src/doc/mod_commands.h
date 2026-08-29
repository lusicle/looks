#pragma once

#include <memory>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

std::unique_ptr<Command> add_value_node_command(uint64_t look,
                                                ValueNode node);
// This also removes every route it feeds and unwires helper inputs.
std::unique_ptr<Command> remove_value_node_command(uint64_t look,
                                                   uint64_t node_id);
// Whole-node replacement: copy the node, then change fields. Coalesces.
std::unique_ptr<Command> set_value_node_command(uint64_t look,
                                                ValueNode node);
// which 0 is input a, which 1 is input b. A `from` of 0 unwires.
// The caller must guard cycles with value_reaches first.
std::unique_ptr<Command> wire_value_input_command(uint64_t look,
                                                  uint64_t node_id,
                                                  int which, uint64_t from);

// One wire per param: a new route replaces the wire of that target.
std::unique_ptr<Command> add_route_command(uint64_t look, ModRoute route);
std::unique_ptr<Command> remove_route_command(uint64_t look,
                                              uint64_t route_id);
std::unique_ptr<Command> set_route_curve_command(uint64_t look,
                                                 uint64_t route_id,
                                                 ResponseCurve curve);

// An empty `keys` removes the lane. This coalesces per target.
std::unique_ptr<Command> set_lane_command(uint64_t look, ParamKey target,
                                          std::vector<Keyframe> keys);

// This coalesces only when the list of targets is the same.
std::unique_ptr<Command> set_lanes_command(uint64_t look,
                                           std::vector<KeyframeLane> lanes);

// The slot is 0 to 2.
std::unique_ptr<Command> store_snapshot_command(uint64_t look, int slot);
// Effects that are gone are skipped. The match is on the effect id.
std::unique_ptr<Command> apply_snapshot_command(uint64_t look, int slot);

// This coalesces, thus a slider drag is one undo step.
std::unique_ptr<Command> set_morph_command(uint64_t look, int from, int to,
                                           float pos);

// trim sets what plays and exports. loop is for the transport only.
std::unique_ptr<Command> set_timeline_region_command(uint64_t sequence,
                                                     uint32_t trim_in,
                                                     uint32_t trim_out,
                                                     uint32_t loop_in,
                                                     uint32_t loop_out);

// This adds the marker if it is absent, or removes it if it is present.
// The marker list stays sorted.
std::unique_ptr<Command> toggle_marker_command(uint64_t sequence,
                                               uint32_t frame);

std::unique_ptr<Command> set_lane_loop_command(uint64_t look, ParamKey target,
                                               bool loop);
// Mute keeps the keys but stops the lane driving its param.
std::unique_ptr<Command> set_lane_mute_command(uint64_t look, ParamKey target,
                                               bool muted);

// This is the speed and mode of the root timeline. It coalesces.
std::unique_ptr<Command> set_time_remap_command(float speed, uint32_t mode);

// A width or height of 0 derives the canvas size from the first asset.
std::unique_ptr<Command> set_project_format_command(double fps, uint32_t w,
                                                    uint32_t h);
// An all-zero format inherits the project format.
std::unique_ptr<Command> set_entity_format_command(uint64_t entity,
                                                   EntityFormat format);

// This coalesces, thus an offset drag is one undo step.
std::unique_ptr<Command> set_audio_config_command(std::string sidechain_path,
                                                  bool sidechain_mux,
                                                  float audio_offset_ms);

// scale is a divisor: a scale of 2 gives half resolution.
std::unique_ptr<Command> set_export_config_command(float bitrate_mbps,
                                                   uint32_t scale,
                                                   bool audio);

// The proxy is half resolution.
std::unique_ptr<Command> set_use_proxy_command(bool use_proxy);

}  // namespace looks::doc
