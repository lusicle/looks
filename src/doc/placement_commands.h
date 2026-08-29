#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// The caller must mint the placement id before this call.
std::unique_ptr<Command> add_placement_command(uint64_t sequence,
                                               uint64_t track_id,
                                               Placement place);

// Timing fields go to the whole link group. Other fields do not.
// This coalesces per placement, thus a block drag is one undo step.
std::unique_ptr<Command> set_placement_command(uint64_t sequence,
                                               Placement updated);

// This clears the link id of every member of the group.
std::unique_ptr<Command> unlink_placement_command(uint64_t sequence,
                                                  uint64_t placement_id);

// This removes the whole link group. The lanes and tracks stay.
std::unique_ptr<Command> remove_placement_command(uint64_t sequence,
                                                  uint64_t placement_id);

// A track_id of 0 mints a new track. A video_placement of 0 does not link.
std::unique_ptr<Command> add_audio_placement_command(
    Document& doc, uint64_t sequence, uint64_t track_id, Placement place,
    uint64_t video_placement);

std::unique_ptr<Command> set_audio_track_props_command(uint64_t sequence,
                                                       uint64_t track_id,
                                                       std::string name,
                                                       float gain,
                                                       bool mute,
                                                       bool lock);

// hidden drops the lane from the composite. The UI honors lock, not this.
std::unique_ptr<Command> set_track_props_command(uint64_t sequence,
                                                 uint64_t track_id,
                                                 std::string name,
                                                 bool hidden, bool lock);

// Link partners do not move. Returns null if the two kinds do not agree.
std::unique_ptr<Command> move_placement_command(const Document& doc,
                                                uint64_t sequence,
                                                uint64_t placement_id,
                                                uint64_t to_track_id);

SeqTrack make_track(Document& doc, const Sequence& seq);
AudioTrack make_audio_track(Document& doc, const Sequence& seq);

// A higher lane index composites later, thus it stacks above.
std::unique_ptr<Command> add_track_command(uint64_t sequence, SeqTrack track,
                                           size_t at_index);
// This removes every placement on the lane too.
// Returns null for the last video lane: a sequence always keeps one.
std::unique_ptr<Command> remove_track_command(const Document& doc,
                                              uint64_t sequence,
                                              uint64_t track_id);
// A sequence can have no audio tracks. A linked pair mints one again.
std::unique_ptr<Command> add_audio_track_command(uint64_t sequence,
                                                 AudioTrack track);
std::unique_ptr<Command> remove_audio_track_command(uint64_t sequence,
                                                    uint64_t track_id);

// A t1 of 0 is unbounded. keep_id and keep_link name the incoming block.
// This executes through `undo`, thus the caller can group the edits.
void overwrite_lane_span(Document& doc, UndoStack& undo, uint64_t sequence,
                         uint64_t track_id, uint64_t keep_id,
                         uint64_t keep_link, uint32_t t0, uint32_t t1);

// Every link partner overwrites the span of its own container.
void overwrite_group_spans(Document& doc, UndoStack& undo,
                           uint64_t sequence, uint64_t placement_id);

}  // namespace looks::doc
