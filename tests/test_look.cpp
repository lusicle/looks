#include "doc/look_commands.h"

#include <cstring>
#include <filesystem>

#include "doc/effects.h"
#include "doc/layer_commands.h"
#include "doc/placement_commands.h"
#include "doc/serialize.h"
#include "doc/stack_commands.h"
#include "doc_fixture.h"
#include "test_framework.h"
#include "util/file.h"

using namespace looks;
using doc::Document;
using doc::EffectType;
using doc::make_effect;

TEST(look_fresh_document_has_a_sequence_and_no_look) {
    Document d;
    CHECK(d.looks.empty());
    CHECK_EQ(d.sequences.size(), size_t{1});
    CHECK_EQ(d.sequences[0].id, d.root_sequence);
    CHECK_EQ(d.sequences[0].tracks.size(), size_t{1});
    CHECK_EQ(d.sequences[0].audio.size(), size_t{1});
}

TEST(look_fixture_ids_are_unique_across_kinds) {
    Document d = doc_with_look();
    CHECK_EQ(d.looks.size(), size_t{1});
    CHECK_EQ(d.looks[0].layers.size(), size_t{1});
    CHECK(doc::layer_is_media(d.looks[0].layers[0]));
    // Ids are unique across kinds: one counter for everything.
    CHECK(d.looks[0].id != d.sequences[0].id);
    CHECK(d.looks[0].id != d.looks[0].layers[0].id);
}

TEST(look_add_remove_undo) {
    Document d = doc_with_look();
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

TEST(look_audio_split_toggle_undo) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    const uint64_t id = d.looks[0].id;
    CHECK(!d.looks[0].audio_split);
    undo.execute(d, doc::set_look_audio_split_command(id, true));
    CHECK(d.looks[0].audio_split);
    undo.undo(d);
    CHECK(!d.looks[0].audio_split);
    undo.redo(d);
    CHECK(d.looks[0].audio_split);

    // Combining DROPS the audio wire; undo brings it back with the mode.
    doc::ensure_links(d.looks[0]);
    d.looks[0].links.push_back({d.looks[0].layers[0].id, 0, 1});
    const size_t wired = d.looks[0].links.size();
    undo.execute(d, doc::set_look_audio_split_command(id, false));
    CHECK(!d.looks[0].audio_split);
    CHECK_EQ(d.looks[0].links.size(), wired - 1);
    for (const doc::NodeLink& l : d.looks[0].links)
        CHECK(!(l.to == 0 && l.to_port == 1));
    undo.undo(d);
    CHECK(d.looks[0].audio_split);
    CHECK_EQ(d.looks[0].links.size(), wired);
}

TEST(look_disconnect_last_wire_stays_deleted) {
    // An empty link table means synthesized wiring, so the delete seals it.
    Document d = doc_with_look();
    d.looks[0].layers[0].asset = d.next_effect_id++;
    doc::UndoStack undo;
    const uint64_t lid = d.looks[0].layers[0].id;
    undo.execute(d, doc::disconnect_command(d.looks[0].id, {lid, 0, 0}));
    CHECK(!d.looks[0].links.empty());
    for (const doc::NodeLink& l : d.looks[0].links)
        CHECK(!(l.to == 0 && l.to_port == 0));
    undo.execute(d, doc::connect_command(d.looks[0].id, {lid, 0, 0}));
    for (const doc::NodeLink& l : d.looks[0].links)
        CHECK(!doc::link_is_tombstone(l));
    undo.undo(d);
    for (const doc::NodeLink& l : d.looks[0].links)
        CHECK(!(l.to == 0 && l.to_port == 0));
    undo.undo(d);
    CHECK(d.looks[0].links.empty());
}

TEST(sequence_add_remove_undo) {
    Document d = doc_with_look();
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

TEST(asset_add_remove_undo) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    doc::Asset a = doc::make_asset(d, "clip", "clip.mp4");
    doc::Asset b = doc::make_asset(d, "roll", "roll.mp4");
    const uint64_t aid = a.id;
    undo.execute(d, doc::add_asset_command(std::move(a)));
    undo.execute(d, doc::add_asset_command(std::move(b)));
    CHECK_EQ(d.assets.size(), size_t{2});

    // The first asset drives auto canvas and fps, so index order matters.
    undo.execute(d, doc::remove_asset_command(aid));
    CHECK_EQ(d.assets.size(), size_t{1});
    CHECK(d.find_asset(aid) == nullptr);
    undo.undo(d);
    CHECK_EQ(d.assets.size(), size_t{2});
    CHECK_EQ(d.assets[0].id, aid);
}

TEST(sequence_track_commands_undo) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    const uint64_t sid = d.sequences[0].id;
    CHECK_EQ(d.sequence(sid).tracks.size(), size_t{1});
    const uint64_t v1 = d.sequence(sid).tracks[0].id;

    // A higher lane index composites later.
    doc::SeqTrack lane = doc::make_track(d, d.sequence(sid));
    const uint64_t v2 = lane.id;
    undo.execute(d, doc::add_track_command(sid, std::move(lane), 1));
    CHECK_EQ(d.sequence(sid).tracks.size(), size_t{2});
    CHECK_EQ(d.sequence(sid).tracks[1].id, v2);

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

    // Clear the default a1 first to exercise the bare add and remove.
    d.sequence(sid).audio.clear();
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
    Document d = doc_with_look();
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

    const uint64_t a = lay(0, 100);
    const uint64_t b = lay(60, 160);
    doc::overwrite_lane_span(d, undo, sid, lane, b, 0, 60, 160);
    CHECK_EQ(doc::find_placement(d.sequence(sid), a)->t_out, uint32_t{60});

    // A newcomer inside splits the block, and source_in holds the content.
    const uint64_t c = lay(20, 50);
    doc::overwrite_lane_span(d, undo, sid, lane, c, 0, 20, 50);
    CHECK_EQ(doc::find_placement(d.sequence(sid), a)->t_out, uint32_t{20});
    uint64_t right = 0;
    for (const doc::Placement& p : d.sequence(sid).tracks[0].placements)
        if (p.t_in == 50 && p.t_out == 60) right = p.id;
    CHECK(right != 0);
    CHECK_EQ(doc::find_placement(d.sequence(sid), right)->source_in,
             uint32_t{50});

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
    Document d = doc_with_look();
    doc::UndoStack undo;
    doc::Bin top = doc::make_bin(d, "top");
    const uint64_t top_id = top.id;
    undo.execute(d, doc::add_bin_command(std::move(top)));
    doc::Bin inner = doc::make_bin(d, "inner");
    inner.parent = top_id;
    const uint64_t inner_id = inner.id;
    undo.execute(d, doc::add_bin_command(std::move(inner)));
    CHECK_EQ(d.bins.size(), size_t{2});

    const uint64_t look_id = d.looks[0].id;
    undo.execute(d, doc::set_entity_bin_command(look_id, inner_id));
    CHECK_EQ(d.looks[0].bin, inner_id);

    CHECK(doc::bin_reaches(d, inner_id, top_id));
    CHECK(!doc::bin_reaches(d, top_id, inner_id));

    // Deleting a bin lifts its contents to the parent bin.
    undo.execute(d, doc::remove_bin_command(inner_id));
    CHECK_EQ(d.bins.size(), size_t{1});
    CHECK_EQ(d.looks[0].bin, top_id);
    undo.undo(d);
    CHECK_EQ(d.bins.size(), size_t{2});
    CHECK_EQ(d.looks[0].bin, inner_id);
    undo.redo(d);
    CHECK_EQ(d.looks[0].bin, top_id);

    undo.execute(d, doc::set_bin_props_command(top_id, "renamed", 0));
    CHECK_EQ(d.find_bin(top_id)->name, "renamed");
    undo.undo(d);
    CHECK_EQ(d.find_bin(top_id)->name, "top");
}

TEST(look_commands_stay_on_their_own_look) {
    // A command captures the look it edits, so undo lands there.
    Document d = doc_with_look();
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

    // 100 target frames left past source_in at 2x fill 50 local frames.
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

    // Speed 0 plays one frame forever, so the end must not wrap uint32.
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
    Document d = doc_with_look();
    doc::Asset a;
    a.id = d.next_effect_id++;
    a.frame_count = 90;
    d.assets.push_back(a);
    d.looks[0].layers[0].asset = a.id;
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{90});
    // Slip shortens what the media can play.
    d.looks[0].layers[0].slip = 30;
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{60});
    // A second, longer media extends the lockstep length.
    doc::Layer l;
    l.id = d.next_effect_id++;
    doc::Asset b;
    b.id = d.next_effect_id++;
    b.frame_count = 200;
    d.assets.push_back(b);
    l.asset = b.id;
    d.looks[0].layers.push_back(std::move(l));
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{200});
    // An explicit duration wins over the derived one.
    d.looks[0].duration = 42;
    CHECK_EQ(doc::look_duration(d, d.looks[0]), uint32_t{42});
}

namespace {

// The rig holds one look, one video block, and a linked audio block.
struct SeqRig {
    Document d = doc_with_look();
    doc::UndoStack undo;
    uint64_t asset = 0;
    uint64_t look = 0;
    uint64_t video = 0;

    explicit SeqRig(uint32_t frames = 200) {
        // Clear the default a1 so the pair lay mints the track on demand.
        d.root().audio.clear();
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

    doc::Placement upd = rig.d.root().tracks[0].placements[0];
    upd.t_in = 25;
    upd.t_out = 125;
    upd.source_in = 5;
    rig.undo.execute(rig.d,
                     doc::set_placement_command(rig.d.root_sequence, upd));
    CHECK_EQ(rig.d.root().audio[0].placements[0].t_in, uint32_t{25});
    CHECK_EQ(rig.d.root().audio[0].placements[0].source_in, uint32_t{5});

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
    CHECK_EQ(rig.d.root().tracks[0].placements[0].link, link);
    CHECK_EQ(rig.d.root().audio[0].placements[0].link, link);
    const uint64_t right_link =
        rig.d.root().tracks[0].placements[1].link;
    CHECK(right_link != 0 && right_link != link);
    CHECK_EQ(rig.d.root().audio[0].placements[1].link, right_link);
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
    Document d = doc_with_look();
    doc::UndoStack undo;
    doc::Look& base = d.looks[0];
    base.layers[0].asset = d.next_effect_id++;
    doc::Layer solid;
    solid.id = d.next_effect_id++;
    solid.source = doc::LayerSourceKind::Solid;
    base.layers.push_back(std::move(solid));
    const uint64_t solid_id = base.layers.back().id;
    const uint64_t media_id = base.layers[0].id;

    auto cmd = doc::nest_layers_command(d, base.id, {solid_id}, "wrap");
    CHECK(cmd != nullptr);
    undo.execute(d, std::move(cmd));
    CHECK_EQ(d.looks.size(), size_t{2});
    CHECK_EQ(d.looks[0].layers.size(), size_t{2});
    const doc::Layer& ref = d.looks[0].layers[1];
    CHECK(ref.source == doc::LayerSourceKind::LookRef);
    const doc::Look* nested = d.find_look(ref.target);
    CHECK(nested != nullptr);
    CHECK_EQ(nested->layers.size(), size_t{1});
    CHECK(nested->layers[0].source == doc::LayerSourceKind::Solid);
    CHECK_EQ(d.looks[0].layers[0].id, media_id);

    undo.undo(d);
    CHECK_EQ(d.looks.size(), size_t{1});
    CHECK_EQ(d.looks[0].layers.size(), size_t{2});
    CHECK_EQ(d.looks[0].layers[1].id, solid_id);
}

TEST(look_make_unique_forks_the_template) {
    SeqRig rig;
    rig.d.looks[0].layers[0].stack.push_back(
        make_effect(rig.d, EffectType::Vignette));
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
    Document d = doc_with_look();
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

TEST(fresh_sequences_carry_a_default_audio_track) {
    Document d = doc_with_look();
    CHECK_EQ(d.root().audio.size(), size_t{1});
    CHECK_EQ(d.root().audio[0].name, "a1");
    CHECK(d.root().audio[0].placements.empty());
    doc::Sequence s = doc::make_sequence(d, "cut");
    CHECK_EQ(s.audio.size(), size_t{1});
    CHECK_EQ(s.audio[0].name, "a1");
}

TEST(lane_props_commands_toggle_hidden_and_lock) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    const uint64_t sid = d.root_sequence;
    const uint64_t lane = d.root().tracks[0].id;
    const uint64_t atrack = d.root().audio[0].id;

    undo.execute(d, doc::set_track_props_command(sid, lane, "v1", true,
                                                 true));
    CHECK(d.root().tracks[0].hidden);
    CHECK(d.root().tracks[0].lock);
    undo.undo(d);
    CHECK(!d.root().tracks[0].hidden);
    CHECK(!d.root().tracks[0].lock);

    undo.execute(d, doc::set_audio_track_props_command(sid, atrack, "a1",
                                                       1.0f, false, true));
    CHECK(d.root().audio[0].lock);
    undo.undo(d);
    CHECK(!d.root().audio[0].lock);
}

TEST(move_placement_lands_on_a_same_kind_lane) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    const uint64_t sid = d.root_sequence;
    const uint64_t v1 = d.root().tracks[0].id;
    doc::SeqTrack lane2 = doc::make_track(d, d.root());
    const uint64_t v2 = lane2.id;
    undo.execute(d, doc::add_track_command(sid, std::move(lane2), 1));
    const uint64_t atrack = d.root().audio[0].id;

    doc::Placement first;
    first.id = d.next_effect_id++;
    first.target = d.looks[0].id;
    first.t_out = 40;
    doc::Placement second;
    second.id = d.next_effect_id++;
    second.target = d.looks[0].id;
    second.t_in = 50;
    second.t_out = 90;
    undo.execute(d, doc::add_placement_command(sid, v1, first));
    undo.execute(d, doc::add_placement_command(sid, v1, second));

    // Cross-kind and no-op targets refuse.
    CHECK(doc::move_placement_command(d, sid, first.id, atrack) == nullptr);
    CHECK(doc::move_placement_command(d, sid, first.id, v1) == nullptr);

    undo.execute(d, doc::move_placement_command(d, sid, first.id, v2));
    CHECK_EQ(d.root().tracks[0].placements.size(), size_t{1});
    CHECK_EQ(d.root().tracks[0].placements[0].id, second.id);
    CHECK_EQ(d.root().tracks[1].placements.size(), size_t{1});
    CHECK_EQ(d.root().tracks[1].placements[0].id, first.id);

    // Undo restores the exact source index (it was FIRST on v1).
    undo.undo(d);
    CHECK_EQ(d.root().tracks[0].placements.size(), size_t{2});
    CHECK_EQ(d.root().tracks[0].placements[0].id, first.id);
    CHECK(d.root().tracks[1].placements.empty());
}

TEST(audio_overwrite_claims_span_like_video) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    const uint64_t sid = d.root_sequence;
    const uint64_t atrack = d.root().audio[0].id;
    const uint64_t target = d.looks[0].id;
    auto lay = [&](uint32_t t_in, uint32_t t_out) {
        doc::Placement p;
        p.id = d.next_effect_id++;
        p.target = target;
        p.t_in = t_in;
        p.t_out = t_out;
        undo.execute(d, doc::add_audio_placement_command(d, sid, atrack,
                                                         p, 0));
        return p.id;
    };

    const uint64_t a = lay(0, 100);
    const uint64_t b = lay(60, 160);
    doc::overwrite_group_spans(d, undo, sid, b);
    CHECK_EQ(d.root().audio[0].placements.size(), size_t{2});
    CHECK_EQ(doc::find_placement(d.root(), a)->t_out, uint32_t{60});

    const uint64_t c = lay(0, 200);
    doc::overwrite_group_spans(d, undo, sid, c);
    CHECK(doc::find_placement(d.root(), a) == nullptr);
    CHECK(doc::find_placement(d.root(), b) == nullptr);
    CHECK_EQ(d.root().audio[0].placements.size(), size_t{1});

    // The right half's source_in slides so the content holds still.
    const uint64_t mid = lay(80, 120);
    doc::overwrite_group_spans(d, undo, sid, mid);
    CHECK_EQ(d.root().audio[0].placements.size(), size_t{3});
    const doc::Placement* left = doc::find_placement(d.root(), c);
    CHECK(left != nullptr);
    CHECK_EQ(left->t_out, uint32_t{80});
    bool found_right = false;
    for (const doc::Placement& p : d.root().audio[0].placements)
        if (p.id != c && p.id != mid) {
            found_right = true;
            CHECK_EQ(p.t_in, uint32_t{120});
            CHECK_EQ(p.source_in, uint32_t{120});
        }
    CHECK(found_right);
}

TEST(linked_pair_lands_claiming_both_lanes) {
    SeqRig rig;
    doc::Placement block;
    block.id = rig.d.next_effect_id++;
    block.target = rig.look;
    block.t_in = 60;
    block.t_out = 160;
    rig.undo.execute(rig.d, doc::add_placement_command(
                                rig.d.root_sequence,
                                rig.d.root().tracks[0].id, block));
    doc::Placement ap;
    ap.target = rig.look;
    ap.t_in = 60;
    ap.t_out = 160;
    rig.undo.execute(rig.d, doc::add_audio_placement_command(
                                rig.d, rig.d.root_sequence,
                                rig.d.root().audio[0].id, ap, block.id));
    doc::overwrite_group_spans(rig.d, rig.undo, rig.d.root_sequence,
                               block.id);
    CHECK_EQ(doc::find_placement(rig.d.root(), rig.video)->t_out,
             uint32_t{60});
    CHECK_EQ(rig.d.root().audio[0].placements[0].t_out, uint32_t{60});
}

TEST(document_survives_losing_every_entity) {
    Document d = doc_with_look();
    doc::UndoStack undo;
    const uint64_t root_id = d.root_sequence;
    CHECK(root_id != 0);
    while (!d.looks.empty())
        undo.execute(d, doc::remove_look_command(d.looks[0].id));
    CHECK(d.looks.empty());
    CHECK_EQ(d.look(root_id).id, uint64_t{0});
    while (!d.sequences.empty())
        undo.execute(d, doc::remove_sequence_command(d.sequences[0].id));
    CHECK(d.sequences.empty());
    CHECK_EQ(d.root_sequence, uint64_t{0});
    CHECK_EQ(d.root().id, uint64_t{0});
    CHECK(d.root().tracks.empty());
    undo.undo(d);
    CHECK_EQ(d.sequences.size(), size_t{1});
    CHECK_EQ(d.root_sequence, root_id);
    undo.undo(d);
    CHECK_EQ(d.looks.size(), size_t{1});
    CHECK_EQ(d.root_sequence, root_id);
}
