// Undoable mutations of the mod matrix, keyframe lanes, and snapshots
// (spec §10: every mutation is a Command). Lane edits are whole-lane
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

// Morph position/endpoints (spec §7). Coalesces (slider drags).
std::unique_ptr<Command> set_morph_command(int from, int to, float pos);

// Time remap (spec §6.1): base speed + playback mode. Coalesces.
std::unique_ptr<Command> set_time_remap_command(float speed, uint32_t mode);

// Timeline region (spec §3/§9): clip trim + transport loop region.
// Coalesces (ruler handle drags).
std::unique_ptr<Command> set_timeline_region_command(uint32_t trim_in,
                                                     uint32_t trim_out,
                                                     uint32_t loop_in,
                                                     uint32_t loop_out);

// Loopable keyframe region toggle (spec §7), per lane target.
std::unique_ptr<Command> set_lane_loop_command(ParamKey target, bool loop);

// Sidechain + audio nudge (spec §7). Coalesces (offset drags).
std::unique_ptr<Command> set_audio_config_command(std::string sidechain_path,
                                                  bool sidechain_mux,
                                                  float audio_offset_ms);

// Half-res proxy toggle (spec §3/§10).
std::unique_ptr<Command> set_use_proxy_command(bool use_proxy);

}  // namespace looks::doc
