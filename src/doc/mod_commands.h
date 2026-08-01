// Undoable mutations of the mod matrix, keyframe lanes, and snapshots
// (every mutation is a Command). Lane edits are whole-lane
// replacements (lanes are tiny) — one command type covers add/move/delete
// key and drags coalesce naturally.

#pragma once

#include <memory>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

std::unique_ptr<Command> add_route_command(ModRoute route);
std::unique_ptr<Command> remove_route_command(uint64_t route_id);
// Coalesces per route id (amount drags = one undo step).
std::unique_ptr<Command> set_route_amount_command(uint64_t route_id,
                                                  float amount);
std::unique_ptr<Command> set_route_source_command(uint64_t route_id,
                                                  ModSource source);
std::unique_ptr<Command> set_route_curve_command(uint64_t route_id,
                                                 ResponseCurve curve);
// Rewires which param the route drives (docs/flow_canvas.md: dropping
// a value node's out wire onto a param row). {0, -1} = unwired (inert).
std::unique_ptr<Command> set_route_target_command(uint64_t route_id,
                                                  ParamKey target);

// Replaces (or creates, or removes when `keys` is empty) the lane for
// `target`. Coalesces per target.
std::unique_ptr<Command> set_lane_command(ParamKey target,
                                          std::vector<Keyframe> keys);

// Atomically replaces several lanes with set_lane_command semantics per
// entry (a bezier point drag writes x and y together; whole-shape keying
// writes 2·N). Coalesces when the target list matches.
std::unique_ptr<Command> set_lanes_command(std::vector<KeyframeLane> lanes);

// Captures the current stack params into snapshot slot 0-2.
std::unique_ptr<Command> store_snapshot_command(int slot);
// Applies slot's values to effects that still exist (matched by id).
std::unique_ptr<Command> apply_snapshot_command(int slot);

// Helper shared with the UI: the snapshot of the current document state.
Snapshot capture_snapshot(const Document& doc);

// Morph position/endpoints. Coalesces (slider drags).
std::unique_ptr<Command> set_morph_command(int from, int to, float pos);

// Time remap: base speed + playback mode. Coalesces.
std::unique_ptr<Command> set_time_remap_command(float speed, uint32_t mode);

// Timeline region: clip trim + transport loop region.
// Coalesces (ruler handle drags).
std::unique_ptr<Command> set_timeline_region_command(uint32_t trim_in,
                                                     uint32_t trim_out,
                                                     uint32_t loop_in,
                                                     uint32_t loop_out);

// Loopable keyframe region toggle, per lane target.
std::unique_ptr<Command> set_lane_loop_command(ParamKey target, bool loop);
// Mute keeps the keys but stops the lane driving its param.
std::unique_ptr<Command> set_lane_mute_command(ParamKey target, bool muted);

// Sidechain + audio nudge. Coalesces (offset drags).
std::unique_ptr<Command> set_audio_config_command(std::string sidechain_path,
                                                  bool sidechain_mux,
                                                  float audio_offset_ms);

// Export settings: bitrate / output scale divisor / audio mute.
std::unique_ptr<Command> set_export_config_command(float bitrate_mbps,
                                                   uint32_t scale,
                                                   bool audio);

// Timeline marker at `frame`: adds when absent, removes when
// present; the list stays sorted.
std::unique_ptr<Command> toggle_marker_command(uint32_t frame);

// Half-res proxy toggle.
std::unique_ptr<Command> set_use_proxy_command(bool use_proxy);

}  // namespace looks::doc
