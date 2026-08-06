// Instance flatten (doc/instances.h): the nesting tree reduced to leaf
// clips in root-local frames.
//
// The load-bearing property is AGREEMENT: the decode pool decodes from
// the flatten while the engine renders from the compiler, and neither
// sees the other. If their keys or their active sets ever diverge, a
// clip shows the wrong frame - so that agreement is tested directly,
// frame by frame.

#include "doc/instances.h"

#include <algorithm>

#include "gfx/graph.h"
#include "test_framework.h"
#include "util/hash.h"

using namespace looks;
using doc::ClipInstance;
using doc::Document;

namespace {

// One asset, one wrapper look (clip node bound to it), one block of
// that look on the root sequence's first lane.
struct Rig {
    Document doc;
    uint64_t asset = 0;
    uint64_t look = 0;
    uint64_t lane = 0;

    explicit Rig(uint32_t frames = 100) {
        doc::Asset a;
        a.id = doc.next_effect_id++;
        a.frame_count = frames;
        doc.assets.push_back(a);
        asset = a.id;
        doc.looks[0].layers[0].asset = asset;
        look = doc.looks[0].id;
        lane = doc.root().tracks[0].id;
        doc::Placement p;
        p.id = doc.next_effect_id++;
        p.target = look;
        doc.root().tracks[0].placements.push_back(p);
    }

    doc::Placement& placement() {
        return doc.root().tracks[0].placements[0];
    }
};

}  // namespace

TEST(flatten_block_spans_its_target) {
    Rig rig(90);
    const auto clips =
        doc::flatten_clip_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(clips.size(), size_t{1});
    const ClipInstance& c = clips[0];
    CHECK_EQ(c.asset, rig.asset);
    CHECK_EQ(c.t_in, 0.0);
    CHECK_EQ(c.t_out, 90.0);
    CHECK_EQ(c.speed, 1.0);
    // The key folds the whole chain: lane, target, clip node, asset.
    const uint64_t path = hash_combine(
        hash_combine(rig.doc.root_sequence, rig.lane), rig.look);
    const uint64_t layer_id = rig.doc.looks[0].layers[0].id;
    CHECK_EQ(c.key,
             hash_combine(hash_combine(path, layer_id), rig.asset));
}

TEST(flatten_composes_block_map_and_slip_in_closed_form) {
    Rig rig(100);
    rig.placement().t_in = 10;
    rig.placement().source_in = 4;
    rig.placement().speed = 2.0f;
    rig.doc.looks[0].layers[0].slip = 6;

    const auto clips =
        doc::flatten_clip_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(clips.size(), size_t{1});
    const ClipInstance& c = clips[0];
    // Root 10 -> look local 4 -> media 10 (slip 6).
    CHECK_EQ(c.t_in, 10.0);
    CHECK_EQ(doc::clip_source_frame(c, 10.0), 10.0);
    CHECK_EQ(doc::clip_source_frame(c, 20.0), 30.0);
    // Media ends at 100: playable 94 past the slip, entered at local 4,
    // so 90 local frames remain = 45 root frames at 2x from t_in 10.
    CHECK_EQ(c.t_out, 55.0);
    CHECK(doc::clip_active(c, 54.9));
    CHECK(!doc::clip_active(c, 55.0));
}

TEST(flatten_and_compiler_agree_frame_by_frame) {
    Rig rig(50);
    rig.placement().t_in = 5;
    rig.placement().t_out = 30;
    // A second block of the same look later on the lane (a razor's
    // shape): distinct windows, the SAME razor-stable key.
    doc::Placement second;
    second.id = rig.doc.next_effect_id++;
    second.target = rig.look;
    second.t_in = 40;
    second.source_in = 10;
    rig.doc.root().tracks[0].placements.push_back(second);

    const auto clips =
        doc::flatten_clip_sources(rig.doc, rig.doc.root_sequence);
    for (uint32_t f = 0; f < 60; f += 3) {
        std::vector<uint64_t> from_flatten;
        for (const ClipInstance& c : clips)
            if (doc::clip_active(c, static_cast<double>(f)))
                from_flatten.push_back(c.key);
        std::vector<uint64_t> from_compile;
        const gfx::RenderGraph g =
            gfx::compile_graph(rig.doc, rig.doc.root_sequence, f);
        for (const gfx::GraphNode& n : g.nodes)
            if (n.kind == gfx::GraphNode::Kind::Source)
                from_compile.push_back(n.key);
        std::sort(from_flatten.begin(), from_flatten.end());
        std::sort(from_compile.begin(), from_compile.end());
        // Duplicate keys collapse (two blocks of one look share a
        // stream); the compiler shows at most one.
        from_flatten.erase(
            std::unique(from_flatten.begin(), from_flatten.end()),
            from_flatten.end());
        CHECK_EQ(from_flatten.size(), from_compile.size());
        for (size_t i = 0;
             i < from_flatten.size() && i < from_compile.size(); ++i)
            CHECK_EQ(from_flatten[i], from_compile[i]);
    }
}

TEST(flatten_same_lane_overlap_emits_both_compiles_the_winner) {
    // Overlap on one lane: the flatten emits BOTH (the pool prewarms
    // both streams), the compiler shows the LATEST-STARTING one.
    Rig rig(100);
    doc::Look second;
    second.id = rig.doc.next_effect_id++;
    doc::Layer clip;
    clip.id = rig.doc.next_effect_id++;
    clip.asset = rig.asset;
    second.layers.push_back(std::move(clip));
    const uint64_t second_id = second.id;
    rig.doc.looks.push_back(std::move(second));
    doc::Placement late;
    late.id = rig.doc.next_effect_id++;
    late.target = second_id;
    late.t_in = 10;
    rig.doc.root().tracks[0].placements.push_back(late);

    const auto clips =
        doc::flatten_clip_sources(rig.doc, rig.doc.root_sequence);
    int active = 0;
    for (const ClipInstance& c : clips)
        if (doc::clip_active(c, 20.0)) ++active;
    CHECK_EQ(active, 2);

    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 20);
    int sources = 0;
    uint64_t shown = 0;
    for (const gfx::GraphNode& n : g.nodes)
        if (n.kind == gfx::GraphNode::Kind::Source) {
            ++sources;
            shown = g.instances[static_cast<size_t>(n.instance)].look;
        }
    CHECK_EQ(sources, 1);
    CHECK_EQ(shown, second_id);
}

TEST(flatten_audio_reads_audio_tracks_only) {
    // Video lanes are silent - sound rides audio placements, and a look
    // target contributes its clips' PCM in lockstep through the map.
    Rig rig(80);
    const auto none =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(none.size(), size_t{0});

    doc::AudioTrack at;
    at.id = rig.doc.next_effect_id++;
    at.gain = 0.5f;
    doc::Placement ap;
    ap.id = rig.doc.next_effect_id++;
    ap.target = rig.look;
    ap.t_in = 10;
    ap.audio_gain = 0.5f;
    at.placements.push_back(ap);
    rig.doc.root().audio.push_back(at);

    const auto mix =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(mix.size(), size_t{1});
    CHECK_EQ(mix[0].asset, rig.asset);
    CHECK_EQ(mix[0].t_in, 10.0);
    CHECK(mix[0].gain > 0.24f && mix[0].gain < 0.26f);

    // Mute anywhere on the path silences the branch entirely.
    rig.doc.root().audio[0].mute = true;
    CHECK_EQ(doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence)
                 .size(),
             size_t{0});
}

TEST(flatten_skips_hidden_and_dangling_sources) {
    Rig rig(50);
    rig.doc.looks[0].layers[0].visible = false;
    CHECK_EQ(
        doc::flatten_clip_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
    rig.doc.looks[0].layers[0].visible = true;
    rig.placement().target = 999999;   // dangling block: dormant
    CHECK_EQ(
        doc::flatten_clip_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
}

TEST(canvas_size_derives_from_the_first_asset) {
    Document d;
    uint32_t w = 0, h = 0;
    doc::canvas_size(d, &w, &h);
    CHECK_EQ(w, uint32_t{1920});
    CHECK_EQ(h, uint32_t{1080});
    doc::Asset a;
    a.id = d.next_effect_id++;
    a.width = 641;   // odd: rounded down to even for NV12
    a.height = 480;
    d.assets.push_back(a);
    doc::canvas_size(d, &w, &h);
    CHECK_EQ(w, uint32_t{640});
    CHECK_EQ(h, uint32_t{480});
    d.canvas_w = 1280;
    d.canvas_h = 720;
    doc::canvas_size(d, &w, &h);
    CHECK_EQ(w, uint32_t{1280});
    CHECK_EQ(h, uint32_t{720});
}

TEST(sequence_duration_is_the_timeline_length) {
    Rig rig(90);
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{90});
    rig.placement().t_in = 20;
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{110});
    // A music bed longer than the picture holds the timeline open.
    doc::AudioTrack at;
    at.id = rig.doc.next_effect_id++;
    doc::Placement ap;
    ap.id = rig.doc.next_effect_id++;
    ap.target = rig.look;
    ap.t_in = 100;
    at.placements.push_back(ap);
    rig.doc.root().audio.push_back(at);
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{190});
    // An explicit duration overrides the derivation.
    rig.doc.root().duration = 42;
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{42});
}
