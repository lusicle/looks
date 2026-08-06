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

}  // namespace looks::doc
