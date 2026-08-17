// Undoable placement edits on a SEQUENCE, applied through the LINK GROUP:
// the video and audio placements one drop laid down edit as ONE - timing
// fields (t_in, t_out, source_in, speed) copy to every linked placement
// so picture and sound cannot drift apart by accident. Unlink is the one
// deliberate desync, which is what a J or L cut is.
//
// A placement lives on a video lane (SeqTrack) or an audio track; these
// commands name it by id and find it wherever it sits.

#pragma once

#include <memory>

#include "doc/command.h"
#include "doc/document.h"

namespace looks::doc {

// Lays a placement on the named video lane - the drop/place path. The
// caller mints the id (it links the audio pair to it); undo removes it.
std::unique_ptr<Command> add_placement_command(uint64_t sequence,
                                               uint64_t track_id,
                                               Placement place);

// Whole-placement replacement matched by id. Timing fields propagate to
// the link group; target and audio fields apply to the named placement
// only. Coalesces per placement, so a block drag is one undo step.
std::unique_ptr<Command> set_placement_command(uint64_t sequence,
                                               Placement updated);

// Dissolves the named placement's link group: every member's link id
// clears. The halves then edit independently - a J cut is two unlinked
// placements, not a special track mode.
std::unique_ptr<Command> unlink_placement_command(uint64_t sequence,
                                                  uint64_t placement_id);

// Removes the named placement AND its whole link group - picture and
// sound leave together, exactly as one drop laid them down. Containers
// (lanes, tracks) stay, even emptied; undo restores every member at its
// exact index. Applies to nothing when the id is gone.
std::unique_ptr<Command> remove_placement_command(uint64_t sequence,
                                                  uint64_t placement_id);

// Mints an audio track and lays a placement on it, linking it to
// `video_placement` when nonzero (the drop path's pair). track_id 0
// mints a new track named "a<n>"; otherwise the placement joins the
// named track.
std::unique_ptr<Command> add_audio_placement_command(
    Document& doc, uint64_t sequence, uint64_t track_id, Placement place,
    uint64_t video_placement);

// Audio track gain/mute/name - the per-track half of the mixer.
std::unique_ptr<Command> set_audio_track_props_command(uint64_t sequence,
                                                       uint64_t track_id,
                                                       std::string name,
                                                       float gain,
                                                       bool mute);

// Fresh empty lanes with minted ids, named by count ("v<n>" / "a<n>").
SeqTrack make_track(Document& doc, const Sequence& seq);
AudioTrack make_audio_track(Document& doc, const Sequence& seq);

// Inserts the fully-formed video lane at `at_index` (clamped; higher
// index composites later = stacks above). Undo removes it.
std::unique_ptr<Command> add_track_command(uint64_t sequence, SeqTrack track,
                                           size_t at_index);
// Removes the lane AND every placement on it; undo restores both at the
// exact index. Null for the sequence's last video lane - the timeline
// keeps one - and for an unknown id.
std::unique_ptr<Command> remove_track_command(const Document& doc,
                                              uint64_t sequence,
                                              uint64_t track_id);
// Same pair for audio tracks; a sequence may run out of them entirely
// (laying a linked pair mints one back on demand). Video partners of
// removed placements keep their link ids - a group of one edits alone.
std::unique_ptr<Command> add_audio_track_command(uint64_t sequence,
                                                 AudioTrack track);
std::unique_ptr<Command> remove_audio_track_command(uint64_t sequence,
                                                    uint64_t track_id);

// OVERWRITE: a landed block claims [t0, t1) on its VIDEO lane - overlaps
// never persist past an edit. What the span covers is tail-trimmed,
// head-trimmed (source_in slides so content holds still), split (razor +
// head-trim) or removed whole (with its link group, the pair rule).
// keep_id/keep_link name the incoming block and its group so it never
// eats itself; t1 0 = unbounded. Executes through `undo` so the caller
// can group it with the edit that landed the block. Audio tracks are
// exempt: summing overlaps is their contract.
void overwrite_lane_span(Document& doc, UndoStack& undo, uint64_t sequence,
                         uint64_t track_id, uint64_t keep_id,
                         uint64_t keep_link, uint32_t t0, uint32_t t1);

}  // namespace looks::doc
