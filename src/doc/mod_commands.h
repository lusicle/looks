// Undoable mutations of the value graph, keyframe lanes, and snapshots
// (every mutation is a Command). Lane edits are whole-lane
// replacements (lanes are tiny) — one command type covers add/move/delete
// key and drags coalesce naturally.
//
// Modulation is LOOK-LOCAL: lanes key on their look's own frames, so
// every command here names the look it edits. The tail of the file —
// playback, audio, export, proxy — is project-wide instead.

#pragma once

#include <memory>
#include <vector>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// ---- value graph: nodes + wires (modulation.h)

std::unique_ptr<Command> add_value_node_command(uint64_t look,
                                                ValueNode node);
// Removing a node also removes every route it feeds and unwires any
// helper input reading it - one undo step restores all of it.
std::unique_ptr<Command> remove_value_node_command(uint64_t look,
                                                   uint64_t node_id);
// Whole-node replace; coalesces per node id (rate/constant drags = one
// undo step). Callers copy the current node and tweak fields.
std::unique_ptr<Command> set_value_node_command(uint64_t look,
                                                ValueNode node);
// Wires helper input a (which 0) or b (which 1) to another node's
// output; 0 unwires. Callers guard cycles with value_reaches FIRST.
std::unique_ptr<Command> wire_value_input_command(uint64_t look,
                                                  uint64_t node_id,
                                                  int which, uint64_t from);

// One wire per param: adding to an already-wired target replaces its
// wire (undo restores it).
std::unique_ptr<Command> add_route_command(uint64_t look, ModRoute route);
std::unique_ptr<Command> remove_route_command(uint64_t look,
                                              uint64_t route_id);
std::unique_ptr<Command> set_route_curve_command(uint64_t look,
                                                 uint64_t route_id,
                                                 ResponseCurve curve);

// Replaces (or creates, or removes when `keys` is empty) the lane for
// `target`. Coalesces per target.
std::unique_ptr<Command> set_lane_command(uint64_t look, ParamKey target,
                                          std::vector<Keyframe> keys);

// Atomically replaces several lanes with set_lane_command semantics per
// entry (a bezier point drag writes x and y together; whole-shape keying
// writes 2·N). Coalesces when the target list matches.
std::unique_ptr<Command> set_lanes_command(uint64_t look,
                                           std::vector<KeyframeLane> lanes);

// Captures the look's current stack params into snapshot slot 0-2.
std::unique_ptr<Command> store_snapshot_command(uint64_t look, int slot);
// Applies slot's values to effects that still exist (matched by id).
std::unique_ptr<Command> apply_snapshot_command(uint64_t look, int slot);

// Helper shared with the UI: the snapshot of a look's current state.
Snapshot capture_snapshot(const Look& look);

// Morph position/endpoints. Coalesces (slider drags).
std::unique_ptr<Command> set_morph_command(uint64_t look, int from, int to,
                                           float pos);

// A sequence's timeline region: trim (what plays and exports) plus the
// transport loop region. Coalesces (ruler handle drags).
std::unique_ptr<Command> set_timeline_region_command(uint64_t sequence,
                                                     uint32_t trim_in,
                                                     uint32_t trim_out,
                                                     uint32_t loop_in,
                                                     uint32_t loop_out);

// Marker at `frame` on the sequence's ruler: adds when absent, removes
// when present; the list stays sorted.
std::unique_ptr<Command> toggle_marker_command(uint64_t sequence,
                                               uint32_t frame);

// Loopable keyframe region toggle, per lane target.
std::unique_ptr<Command> set_lane_loop_command(uint64_t look, ParamKey target,
                                               bool loop);
// Mute keeps the keys but stops the lane driving its param.
std::unique_ptr<Command> set_lane_mute_command(uint64_t look, ParamKey target,
                                               bool muted);

// ---- project-wide settings (not scoped to a look)

// Time remap: base speed + playback mode on the root timeline. Coalesces.
std::unique_ptr<Command> set_time_remap_command(float speed, uint32_t mode);

// Project format: frame rate + canvas size (0 = derive from the first
// asset). The default clock and canvas for every entity.
std::unique_ptr<Command> set_project_format_command(double fps, uint32_t w,
                                                    uint32_t h);
// Per-entity format (look or sequence): all-zero = inherit the project.
std::unique_ptr<Command> set_entity_format_command(uint64_t entity,
                                                   EntityFormat format);

// Sidechain + audio nudge. Coalesces (offset drags).
std::unique_ptr<Command> set_audio_config_command(std::string sidechain_path,
                                                  bool sidechain_mux,
                                                  float audio_offset_ms);

// Export settings: bitrate / output scale divisor / audio mute.
std::unique_ptr<Command> set_export_config_command(float bitrate_mbps,
                                                   uint32_t scale,
                                                   bool audio);

// Half-res proxy toggle.
std::unique_ptr<Command> set_use_proxy_command(bool use_proxy);

}  // namespace looks::doc
