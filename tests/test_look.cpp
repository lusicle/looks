// The two-entity model: looks as timeless templates, sequences as the
// only timelines, placements as affine maps, link groups, the nesting
// guard, and the version gate.

#include "doc/look_commands.h"

#include <cstring>
#include <filesystem>

#include "doc/effects.h"
#include "doc/layer_commands.h"
#include "doc/placement_commands.h"
#include "doc/serialize.h"
#include "doc/stack_commands.h"
#include "test_framework.h"
#include "util/file.h"

using namespace looks;
using doc::Document;
using doc::EffectType;
using doc::make_effect;

TEST(look_fresh_document_has_look_and_sequence) {
    Document d;
    CHECK_EQ(d.looks.size(), size_t{1});
    CHECK_EQ(d.sequences.size(), size_t{1});
    CHECK_EQ(d.sequences[0].id, d.root_sequence);
    CHECK_EQ(d.sequences[0].tracks.size(), size_t{1});
    CHECK_EQ(d.looks[0].layers.size(), size_t{1});
    CHECK(doc::layer_is_clip(d.looks[0].layers[0]));
    // Ids are unique across kinds: one counter for everything.
    CHECK(d.looks[0].id != d.sequences[0].id);
    CHECK(d.looks[0].id != d.looks[0].layers[0].id);
}

TEST(look_add_remove_undo) {
    Document d;
    doc::UndoStack undo;
    doc::Look fresh = doc::make_look(d, "second");
    const uint64_t id = fresh.id;
    undo.execute(d, doc::add_look_command(std::move(fresh)));
    CHECK_EQ(d.looks.size(), size_t{2});

    undo.execute(d, doc::set_look_props_command(id, "renamed", 120));
    CHECK_EQ(d.look(id).name, "renamed");
    CHECK_EQ(d.look(id).duration, uint32_t{120});
    undo.undo(d);
    CHECK_EQ(d.look(id).name, "second");

    undo.execute(d, doc::remove_look_command(id));
    CHECK_EQ(d.looks.size(), size_t{1});
    undo.undo(d);
    CHECK_EQ(d.looks.size(), size_t{2});
    CHECK_EQ(d.look(id).name, "second");
}

TEST(sequence_add_remove_undo) {
    Document d;
    doc::UndoStack undo;
    doc::Sequence fresh = doc::make_sequence(d, "cut 2");
    const uint64_t id = fresh.id;
    CHECK_EQ(fresh.tracks.size(), size_t{1});
    undo.execute(d, doc::add_sequence_command(std::move(fresh)));
    CHECK_EQ(d.sequences.size(), size_t{2});

    undo.execute(d, doc::set_sequence_props_command(id, "final", 300));
    CHECK_EQ(d.sequence(id).name, "final");
    CHECK_EQ(d.sequence(id).duration, uint32_t{300});
    undo.undo(d);
    CHECK_EQ(d.sequence(id).name, "cut 2");

    undo.execute(d, doc::remove_sequence_command(id));
    CHECK_EQ(d.sequences.size(), size_t{1});
    undo.undo(d);
    CHECK_EQ(d.sequences.size(), size_t{2});
}

TEST(sequence_track_commands_undo) {
    Document d;
    doc::UndoStack undo;
    const uint64_t sid = d.sequences[0].id;
    CHECK_EQ(d.sequence(sid).tracks.size(), size_t{1});
    const uint64_t v1 = d.sequence(sid).tracks[0].id;

    // Lanes insert at an index (higher composites later) and undo out.
    doc::SeqTrack lane = doc::make_track(d, d.sequence(sid));
    const uint64_t v2 = lane.id;
    undo.execute(d, doc::add_track_command(sid, std::move(lane), 1));
    CHECK_EQ(d.sequence(sid).tracks.size(), size_t{2});
    CHECK_EQ(d.sequence(sid).tracks[1].id, v2);

    // A removed lane takes its placements; undo restores both in place.
    doc::Placement p;
    p.id = d.next_effect_id++;
    p.target = d.looks[0].id;
    p.t_in = 0;
    p.t_out = 10;
    undo.execute(d, doc::add_placement_command(sid, v2, p));
    undo.execute(d, doc::remove_track_command(d, sid, v2));
    CHECK_EQ(d.sequence(sid).tracks.size(), size_t{1});
    CHECK_EQ(d.sequence(sid).tracks[0].id, v1);
    undo.undo(d);
    CHECK_EQ(d.sequence(sid).tracks.size(), size_t{2});
    CHECK_EQ(d.sequence(sid).tracks[1].placements.size(), size_t{1});

    // The last video lane refuses removal.
    undo.execute(d, doc::remove_track_command(d, sid, v2));
    CHECK(doc::remove_track_command(d, sid, v1) == nullptr);

    // Audio tracks add bare and remove with their placements.
    CHECK_EQ(d.sequence(sid).audio.size(), size_t{0});
    doc::AudioTrack at = doc::make_audio_track(d, d.sequence(sid));
    const uint64_t a1 = at.id;
    undo.execute(d, doc::add_audio_track_command(sid, std::move(at)));
    CHECK_EQ(d.sequence(sid).audio.size(), size_t{1});
    doc::Placement ap;
    ap.target = d.looks[0].id;
    ap.t_in = 0;
    ap.t_out = 10;
    undo.execute(d, doc::add_audio_placement_command(d, sid, a1, ap, 0));
    CHECK_EQ(d.sequence(sid).audio[0].placements.size(), size_t{1});
    undo.execute(d, doc::remove_audio_track_command(sid, a1));
    CHECK_EQ(d.sequence(sid).audio.size(), size_t{0});
    undo.undo(d);
    CHECK_EQ(d.sequence(sid).audio.size(), size_t{1});
    CHECK_EQ(d.sequence(sid).audio[0].placements.size(), size_t{1});
}

TEST(sequence_overwrite_claims_span) {
    Document d;
    doc::UndoStack undo;
    const uint64_t sid = d.sequences[0].id;
    const uint64_t lane = d.sequence(sid).tracks[0].id;
    const uint64_t target = d.looks[0].id;
    auto lay = [&](uint32_t t_in, uint32_t t_out) {
        doc::Placement p;
        p.id = d.next_effect_id++;
        p.target = target;
        p.t_in = t_in;
        p.t_out = t_out;
        undo.execute(d, doc::add_placement_command(sid, lane, p));
        return p.id;
    };

    // Tail under the newcomer: cut to its start.
    const uint64_t a = lay(0, 100);
    const uint64_t b = lay(60, 160);
    doc::overwrite_lane_span(d, undo, sid, lane, b, 0, 60, 160);
    CHECK_EQ(doc::find_placement(d.sequence(sid), a)->t_out, uint32_t{60});

    // Newcomer strictly inside: split (razor + head-trim), content holds
    // still through source_in.
    const uint64_t c = lay(20, 50);
    doc::overwrite_lane_span(d, undo, sid, lane, c, 0, 20, 50);
    CHECK_EQ(doc::find_placement(d.sequence(sid), a)->t_out, uint32_t{20});
    uint64_t right = 0;
    for (const doc::Placement& p : d.sequence(sid).tracks[0].placements)
        if (p.t_in == 50 && p.t_out == 60) right = p.id;
    CHECK(right != 0);
    CHECK_EQ(doc::find_placement(d.sequence(sid), right)->source_in,
             uint32_t{50});

    // One landing that tail-trims, removes whole, and head-trims at once;
    // a single undo of the group restores all three.
    const uint64_t e = lay(45, 70);
    undo.begin_group("Overwrite");
    doc::overwrite_lane_span(d, undo, sid, lane, e, 0, 45, 70);
    undo.end_group();
    CHECK_EQ(doc::find_placement(d.sequence(sid), c)->t_out, uint32_t{45});
    CHECK(doc::find_placement(d.sequence(sid), right) == nullptr);
    CHECK_EQ(doc::find_placement(d.sequence(sid), b)->t_in, uint32_t{70});
    CHECK_EQ(doc::find_placement(d.sequence(sid), b)->source_in,
             uint32_t{10});
    undo.undo(d);
    CHECK(doc::find_placement(d.sequence(sid), right) != nullptr);
    CHECK_EQ(doc::find_placement(d.sequence(sid), b)->t_in, uint32_t{60});
    CHECK_EQ(doc::find_placement(d.sequence(sid), c)->t_out, uint32_t{50});
}

TEST(bin_commands_organise_the_browser) {
    Document d;
    doc::UndoStack undo;
    doc::Bin top = doc::make_bin(d, "top");
    const uint64_t top_id = top.id;
    undo.execute(d, doc::add_bin_command(std::move(top)));
    doc::Bin inner = doc::make_bin(d, "inner");
    inner.parent = top_id;
    const uint64_t inner_id = inner.id;
    undo.execute(d, doc::add_bin_command(std::move(inner)));
    CHECK_EQ(d.bins.size(), size_t{2});

    // Membership: file the first look inside the inner bin.
    const uint64_t look_id = d.looks[0].id;
    undo.execute(d, doc::set_entity_bin_command(look_id, inner_id));
    CHECK_EQ(d.looks[0].bin, inner_id);

    // The reparent guard walks parents: inner sits under top, never the
    // other way.
    CHECK(doc::bin_reaches(d, inner_id, top_id));
    CHECK(!doc::bin_reaches(d, top_id, inner_id));

    // Deleting a bin lifts its contents to its parent; undo restores.
    undo.execute(d, doc::remove_bin_command(inner_id));
    CHECK_EQ(d.bins.size(), size_t{1});
    CHECK_EQ(d.looks[0].bin, top_id);
    undo.undo(d);
    CHECK_EQ(d.bins.size(), size_t{2});
    CHECK_EQ(d.looks[0].bin, inner_id);
    undo.redo(d);
    CHECK_EQ(d.looks[0].bin, top_id);

    // Rename + reparent ride one command; undo restores both.
    undo.execute(d, doc::set_bin_props_command(top_id, "renamed", 0));
    CHECK_EQ(d.find_bin(top_id)->name, "renamed");
    undo.undo(d);
    CHECK_EQ(d.find_bin(top_id)->name, "top");
}

TEST(look_commands_stay_on_their_own_look) {
    // A command captures the look it edits: undo must land there no
    // matter what the editing scope moved to afterwards.
    Document d;
    doc::UndoStack undo;
    doc::Look second = doc::make_look(d, "second");
    doc::Layer l;
    l.id = d.next_effect_id++;
    second.layers.push_back(std::move(l));
    const uint64_t second_id = second.id;
    undo.execute(d, doc::add_look_command(std::move(second)));

    undo.execute(d, doc::add_effect_command(
                        d.looks[0].id, 0,
                        make_effect(d, EffectType::Vignette), 0));
    undo.execute(d, doc::add_effect_command(
                        second_id, 0, make_effect(d, EffectType::Grain), 0));
    CHECK_EQ(d.looks[0].layers[0].stack.size(), size_t{1});
    CHECK_EQ(d.look(second_id).layers[0].stack.size(), size_t{1});

    undo.undo(d);
    CHECK_EQ(d.look(second_id).layers[0].stack.size(), size_t{0});
    CHECK_EQ(d.looks[0].layers[0].stack.size(), size_t{1});
    undo.undo(d);
    CHECK_EQ(d.looks[0].layers[0].stack.size(), size_t{0});
}

TEST(look_placement_maps_local_to_source) {
    doc::Placement p;
    CHECK_EQ(doc::placement_source_frame(p, 30.0), 30.0);

    p.t_in = 10;
    p.source_in = 100;
    p.speed = 2.0f;
    CHECK_EQ(doc::placement_source_frame(p, 10.0), 100.0);
    CHECK_EQ(doc::placement_source_frame(p, 20.0), 120.0);

    // Unbounded (t_out 0) with a known target length: the 100 target
    // frames REMAINING past source_in, at 2x, occupy 50 local frames.
    CHECK_EQ(doc::placement_end(p, 200), uint32_t{60});
    CHECK(!doc::placement_active(p, 200, 9));
    CHECK(doc::placement_active(p, 200, 10));
    CHECK(doc::placement_active(p, 200, 59));
    CHECK(!doc::placement_active(p, 200, 60));
    p.t_out = 50;
    CHECK_EQ(doc::placement_end(p, 200), uint32_t{50});
    doc::Placement open;
    CHECK_EQ(doc::placement_end(open, 0), uint32_t{0});
    CHECK(doc::placement_active(open, 0, 100000));

    // A FROZEN placement (speed 0) plays one frame forever: its end must
    // stay a usable frame index rather than wrapping uint32.
    doc::Placement frozen;
    frozen.speed = 0.0f;
    const uint32_t fend = doc::placement_end(frozen, 5000);
    CHECK(fend > 0);
    CHECK(fend <= uint32_t{1000000000});
    doc::Placement past;
    past.t_in = 7;
    past.source_in = 900;
    CHECK_EQ(doc::placement_end(past, 100), uint32_t{8});
}

TEST(look_duration_is_lockstep_content) {
    Document d;
    doc::Asset a;
    a.id = d.next_effect_id++;
    a.frame_count = 90;
    d.assets.push_back(a);
    d.looks[0].layers[0].asset = a.id;
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{90});
    // Slip shortens what the clip can play.
    d.looks[0].layers[0].slip = 30;
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{60});
    // A second, longer clip extends the lockstep length.
    doc::Layer l;
    l.id = d.next_effect_id++;
    doc::Asset b;
    b.id = d.next_effect_id++;
    b.frame_count = 200;
    d.assets.push_back(b);
    l.asset = b.id;
    d.looks[0].layers.push_back(std::move(l));
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{200});
    // Generator-only looks derive 0 = unbounded; explicit wins.
    d.looks[0].duration = 42;
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{42});
}

namespace {

// One asset, its wrapper look, one block on the root lane, and a linked
// audio placement - the shape one drop lays down.
struct SeqRig {
    Document d;
    doc::UndoStack undo;
    uint64_t asset = 0;
    uint64_t look = 0;
    uint64_t video = 0;

    explicit SeqRig(uint32_t frames = 200) {
        doc::Asset a;
        a.id = d.next_effect_id++;
        a.frame_count = frames;
        d.assets.push_back(a);
        asset = a.id;
        d.looks[0].layers[0].asset = asset;
        look = d.looks[0].id;
        doc::Placement block;
        block.id = d.next_effect_id++;
        block.target = look;
        block.t_out = 100;
        video = block.id;
        undo.execute(d, doc::add_placement_command(
                            d.root_sequence, d.root().tracks[0].id, block));
        doc::Placement ap;
        ap.target = look;
        ap.t_out = 100;
        undo.execute(d, doc::add_audio_placement_command(
                            d, d.root_sequence, 0, ap, video));
    }
};

}  // namespace

TEST(sequence_audio_placements_pair_and_edit_as_one) {
    SeqRig rig;
    CHECK_EQ(rig.d.root().audio.size(), size_t{1});
    CHECK_EQ(rig.d.root().audio[0].placements.size(), size_t{1});
    const uint64_t link = rig.d.root().tracks[0].placements[0].link;
    CHECK(link != 0);
    CHECK_EQ(rig.d.root().audio[0].placements[0].link, link);

    // A timing edit through EITHER member moves both.
    doc::Placement upd = rig.d.root().tracks[0].placements[0];
    upd.t_in = 25;
    upd.t_out = 125;
    upd.source_in = 5;
    rig.undo.execute(rig.d,
                     doc::set_placement_command(rig.d.root_sequence, upd));
    CHECK_EQ(rig.d.root().audio[0].placements[0].t_in, uint32_t{25});
    CHECK_EQ(rig.d.root().audio[0].placements[0].source_in, uint32_t{5});

    // A level edit on the audio half touches only it.
    doc::Placement aupd = rig.d.root().audio[0].placements[0];
    aupd.audio_gain = 0.25f;
    rig.undo.execute(rig.d,
                     doc::set_placement_command(rig.d.root_sequence, aupd));
    CHECK_EQ(rig.d.root().audio[0].placements[0].audio_gain, 0.25f);
    CHECK_EQ(rig.d.root().tracks[0].placements[0].audio_gain, 1.0f);
    CHECK_EQ(rig.d.root().tracks[0].placements[0].t_in, uint32_t{25});

    rig.undo.undo(rig.d);
    CHECK_EQ(rig.d.root().audio[0].placements[0].audio_gain, 1.0f);
    rig.undo.undo(rig.d);
    CHECK_EQ(rig.d.root().tracks[0].placements[0].t_in, uint32_t{0});
    CHECK_EQ(rig.d.root().audio[0].placements[0].t_in, uint32_t{0});

    // Unlink dissolves the group: the same timing edit now moves one.
    rig.undo.execute(
        rig.d, doc::unlink_placement_command(rig.d.root_sequence,
                                             rig.video));
    doc::Placement solo = rig.d.root().tracks[0].placements[0];
    solo.t_in = 60;
    rig.undo.execute(rig.d,
                     doc::set_placement_command(rig.d.root_sequence, solo));
    CHECK_EQ(rig.d.root().tracks[0].placements[0].t_in, uint32_t{60});
    CHECK_EQ(rig.d.root().audio[0].placements[0].t_in, uint32_t{0});
}

TEST(sequence_razor_splits_the_whole_link_group) {
    SeqRig rig;
    const uint64_t lane = rig.d.root().tracks[0].id;
    const uint64_t atrack = rig.d.root().audio[0].id;
    const uint64_t link = rig.d.root().tracks[0].placements[0].link;

    rig.undo.execute(rig.d, doc::razor_track_command(
                                rig.d, rig.d.root_sequence, lane, 40));
    CHECK_EQ(rig.d.root().tracks[0].placements.size(), size_t{2});
    CHECK_EQ(rig.d.root().audio[0].placements.size(), size_t{2});
    // Left halves keep the original pair; right halves pair fresh.
    CHECK_EQ(rig.d.root().tracks[0].placements[0].link, link);
    CHECK_EQ(rig.d.root().audio[0].placements[0].link, link);
    const uint64_t right_link =
        rig.d.root().tracks[0].placements[1].link;
    CHECK(right_link != 0 && right_link != link);
    CHECK_EQ(rig.d.root().audio[0].placements[1].link, right_link);
    // The halves are continuous: right resumes at the cut's source.
    CHECK_EQ(rig.d.root().tracks[0].placements[0].t_out, uint32_t{40});
    CHECK_EQ(rig.d.root().tracks[0].placements[1].t_in, uint32_t{40});
    CHECK_EQ(rig.d.root().tracks[0].placements[1].source_in, uint32_t{40});

    // The audio razor path is idempotent against already-cut groups.
    CHECK(!doc::razor_audio_command(rig.d, rig.d.root_sequence, atrack,
                                    40));
    rig.undo.undo(rig.d);
    CHECK_EQ(rig.d.root().tracks[0].placements.size(), size_t{1});
    CHECK_EQ(rig.d.root().audio[0].placements.size(), size_t{1});
    CHECK_EQ(rig.d.root().tracks[0].placements[0].t_out, uint32_t{100});
}

TEST(sequence_remove_placement_takes_the_link_group) {
    SeqRig rig;
    // A second, unlinked block later on the lane.
    doc::Placement second;
    second.id = rig.d.next_effect_id++;
    second.target = rig.look;
    second.t_in = 120;
    second.t_out = 150;
    rig.undo.execute(rig.d, doc::add_placement_command(
                                rig.d.root_sequence,
                                rig.d.root().tracks[0].id, second));

    rig.undo.execute(rig.d, doc::remove_placement_command(
                                rig.d.root_sequence, rig.video));
    CHECK_EQ(rig.d.root().tracks[0].placements.size(), size_t{1});
    CHECK_EQ(rig.d.root().tracks[0].placements[0].id, second.id);
    CHECK_EQ(rig.d.root().audio.size(), size_t{1});
    CHECK(rig.d.root().audio[0].placements.empty());

    rig.undo.undo(rig.d);
    CHECK_EQ(rig.d.root().tracks[0].placements.size(), size_t{2});
    CHECK_EQ(rig.d.root().tracks[0].placements[0].id, rig.video);
    CHECK_EQ(rig.d.root().audio[0].placements.size(), size_t{1});
}

TEST(look_nest_wraps_graph_selection_in_a_lockstep_ref) {
    Document d;
    doc::UndoStack undo;
    doc::Look& base = d.looks[0];
    base.layers[0].asset = d.next_effect_id++;
    doc::Layer solid;
    solid.id = d.next_effect_id++;
    solid.source = doc::LayerSourceKind::Solid;
    base.layers.push_back(std::move(solid));
    const uint64_t solid_id = base.layers.back().id;
    const uint64_t clip_id = base.layers[0].id;

    auto cmd = doc::nest_layers_command(d, base.id, {solid_id}, "wrap");
    CHECK(cmd != nullptr);
    undo.execute(d, std::move(cmd));
    CHECK_EQ(d.looks.size(), size_t{2});
    // The ref stands where the solid stood, in lockstep.
    CHECK_EQ(d.looks[0].layers.size(), size_t{2});
    const doc::Layer& ref = d.looks[0].layers[1];
    CHECK(ref.source == doc::LayerSourceKind::LookRef);
    const doc::Look* nested = d.find_look(ref.target);
    CHECK(nested != nullptr);
    CHECK_EQ(nested->layers.size(), size_t{1});
    CHECK(nested->layers[0].source == doc::LayerSourceKind::Solid);
    // The parent kept its other chain; the clip stayed put.
    CHECK_EQ(d.looks[0].layers[0].id, clip_id);

    undo.undo(d);
    CHECK_EQ(d.looks.size(), size_t{1});
    CHECK_EQ(d.looks[0].layers.size(), size_t{2});
    CHECK_EQ(d.looks[0].layers[1].id, solid_id);
}

TEST(look_make_unique_forks_the_template) {
    SeqRig rig;
    rig.d.looks[0].layers[0].stack.push_back(
        make_effect(rig.d, EffectType::Vignette));
    // A second block sharing the template.
    doc::Placement dup;
    dup.id = rig.d.next_effect_id++;
    dup.target = rig.look;
    dup.t_in = 120;
    rig.undo.execute(rig.d, doc::add_placement_command(
                                rig.d.root_sequence,
                                rig.d.root().tracks[0].id, dup));

    auto cmd = doc::make_unique_command(rig.d, rig.d.root_sequence, dup.id);
    CHECK(cmd != nullptr);
    rig.undo.execute(rig.d, std::move(cmd));
    CHECK_EQ(rig.d.looks.size(), size_t{2});
    const uint64_t fork =
        rig.d.root().tracks[0].placements[1].target;
    CHECK(fork != rig.look);
    // The first block keeps the original; the fork carries the stack
    // with reminted ids.
    CHECK_EQ(rig.d.root().tracks[0].placements[0].target, rig.look);
    const doc::Look* forked = rig.d.find_look(fork);
    CHECK(forked != nullptr);
    CHECK_EQ(forked->layers[0].stack.size(), size_t{1});
    CHECK(forked->layers[0].stack[0].id !=
          rig.d.looks[0].layers[0].stack[0].id);

    rig.undo.undo(rig.d);
    CHECK_EQ(rig.d.looks.size(), size_t{1});
    CHECK_EQ(rig.d.root().tracks[0].placements[1].target, rig.look);
}

TEST(sequence_make_unique_forks_a_nested_cut) {
    Document d;
    doc::UndoStack undo;
    doc::Sequence cut = doc::make_sequence(d, "cut");
    const uint64_t cut_id = cut.id;
    doc::Placement inner;
    inner.id = d.next_effect_id++;
    inner.target = d.looks[0].id;
    inner.t_out = 50;
    cut.tracks[0].placements.push_back(inner);
    undo.execute(d, doc::add_sequence_command(std::move(cut)));
    doc::Placement block;
    block.id = d.next_effect_id++;
    block.target = cut_id;
    undo.execute(d, doc::add_placement_command(
                        d.root_sequence, d.root().tracks[0].id, block));

    auto cmd = doc::make_unique_command(d, d.root_sequence, block.id);
    CHECK(cmd != nullptr);
    undo.execute(d, std::move(cmd));
    CHECK_EQ(d.sequences.size(), size_t{3});
    const uint64_t fork = d.root().tracks[0].placements[0].target;
    CHECK(fork != cut_id);
    const doc::Sequence* forked = d.find_sequence(fork);
    CHECK(forked != nullptr);
    // The fork's placements reminted but still target the SHARED look.
    CHECK_EQ(forked->tracks[0].placements.size(), size_t{1});
    CHECK_EQ(forked->tracks[0].placements[0].target, d.looks[0].id);
    CHECK(forked->tracks[0].placements[0].id != inner.id);
    undo.undo(d);
    CHECK_EQ(d.sequences.size(), size_t{2});
}

TEST(look_version_gate_refuses_older_projects) {
    const std::filesystem::path path =
        std::filesystem::path(LOOKS_REPO_ROOT) / "temp" /
        "version_gate_test.json";
    const char* old_file =
        "{\"looks_project\": 4, \"name\": \"old\", \"looks\": []}";
    CHECK(looks::write_file_bytes(path, old_file, std::strlen(old_file)));
    std::string error;
    auto loaded = doc::load_document(path, &error);
    CHECK(!loaded.has_value());
    CHECK(error.find("value graph") != std::string::npos);
}
