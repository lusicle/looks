// Instance flatten (doc/instances.h): the nesting tree reduced to leaf
// sources in root-local frames.
//
// The load-bearing property is AGREEMENT: the decode pool decodes from
// the flatten while the engine renders from the compiler, and neither
// sees the other. If their keys or their active sets ever diverge, a
// source shows the wrong frame - so that agreement is tested directly,
// frame by frame.

#include "doc/instances.h"

#include <algorithm>
#include <cmath>

#include "doc/command.h"
#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/stack_commands.h"
#include "gfx/graph.h"
#include "test_framework.h"
#include "util/hash.h"

using namespace looks;
using doc::MediaInstance;
using doc::Document;

namespace {

// One asset, one wrapper look (media node bound to it), one block of
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
    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    const MediaInstance& c = sources[0];
    CHECK_EQ(c.asset, rig.asset);
    CHECK_EQ(c.t_in, 0.0);
    CHECK_EQ(c.t_out, 90.0);
    CHECK_EQ(c.speed, 1.0);
    // The key folds the whole chain: lane, target, media node, asset.
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

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    const MediaInstance& c = sources[0];
    // Root 10 -> look local 4 -> media 10 (slip 6 applied post-rate).
    CHECK_EQ(c.t_in, 10.0);
    CHECK_EQ(doc::media_asset_frame(c, 10.0), 10.0);
    CHECK_EQ(doc::media_asset_frame(c, 20.0), 30.0);
    // Media ends at 100: playable 94 past the slip, entered at local 4,
    // so 90 local frames remain = 45 root frames at 2x from t_in 10.
    CHECK_EQ(c.t_out, 55.0);
    CHECK(doc::media_active(c, 54.9));
    CHECK(!doc::media_active(c, 55.0));
}

TEST(flatten_offset_shim_emits_a_shifted_stream) {
    // An Offset wired directly onto a media node adds a SHIFTED read
    // of that source: its own window, its own stream key, and the
    // compiler consumes the shifted head while the base one idles.
    Rig rig(100);
    doc::Look& look = rig.doc.looks[0];
    const uint64_t layer_id = look.layers[0].id;
    doc::EffectInstance off =
        doc::make_effect(rig.doc, doc::EffectType::Offset);
    off.params[0] = 10.0f;   // +10 frames
    off.params[1] = 2.0f;    // both
    const uint64_t off_id = off.id;
    look.layers[0].stack.push_back(off);
    look.links = {{layer_id, off_id, 0}, {off_id, 0, 0}};
    {
        doc::AudioTrack at;
        at.id = rig.doc.next_effect_id++;
        doc::Placement ap;
        ap.id = rig.doc.next_effect_id++;
        ap.target = rig.look;
        at.placements.push_back(ap);
        rig.doc.root().audio.clear();
        rig.doc.root().audio.push_back(at);
    }

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{2});
    if (sources.size() < 2) return;
    const MediaInstance& base = sources[0];
    const MediaInstance& sh = sources[1];
    CHECK_EQ(doc::media_asset_frame(base, 0.0), 0.0);
    CHECK_EQ(doc::media_asset_frame(sh, 0.0), 10.0);
    CHECK_EQ(sh.t_out, 90.0);   // 100-frame media, entered 10 in
    CHECK(sh.key != base.key);

    // The composite consumes the SHIFTED stream (the Output wire runs
    // through the shim), and both keys agree with the flatten's.
    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 20);
    std::vector<uint64_t> compiled;
    for (const gfx::GraphNode& n : g.nodes)
        if (n.kind == gfx::GraphNode::Kind::Source)
            compiled.push_back(n.key);
    CHECK_EQ(compiled.size(), size_t{2});
    CHECK(std::find(compiled.begin(), compiled.end(), base.key) !=
          compiled.end());
    CHECK(std::find(compiled.begin(), compiled.end(), sh.key) !=
          compiled.end());
    CHECK_EQ(g.nodes[static_cast<size_t>(g.output)].key, sh.key);

    // Past the shifted media's end the shim is a CLOSED GATE: the
    // flatten window closes and the compiler's composite goes black
    // while the (unconsumed) base head still exists.
    CHECK(!doc::media_active(sh, 95.0));
    CHECK(doc::media_active(base, 95.0));
    const gfx::RenderGraph g2 =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 95);
    CHECK_EQ(g2.nodes[static_cast<size_t>(g2.output)].kind,
             gfx::GraphNode::Kind::Generator);

    // The AUDIO walk applies the same shift to the voice.
    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    if (voice.empty()) return;
    CHECK_EQ(doc::media_asset_frame(voice[0], 0.0), 10.0);

    // The closed-form chain the analysis keys on carries it too.
    const doc::AudioChain chain =
        doc::resolve_audio_chain(rig.doc, look, off_id);
    CHECK_EQ(chain.asset, rig.asset);
    CHECK_EQ(chain.offset, int64_t{10});
    CHECK(!chain.locked);
    look.layers[0].timeline_lock = true;
    CHECK(doc::resolve_audio_chain(rig.doc, look, off_id).locked);
}

TEST(flatten_offset_shim_off_the_source_is_inert) {
    // Wired mid-chain (behind an effect) the shim passes through: no
    // shifted stream, no audio shift - it applies ONLY sitting directly
    // on a source node.
    Rig rig(100);
    doc::Look& look = rig.doc.looks[0];
    const uint64_t layer_id = look.layers[0].id;
    doc::EffectInstance fx =
        doc::make_effect(rig.doc, doc::EffectType::Vignette);
    doc::EffectInstance off =
        doc::make_effect(rig.doc, doc::EffectType::Offset);
    off.params[0] = 10.0f;
    off.params[1] = 2.0f;
    const uint64_t fx_id = fx.id;
    const uint64_t off_id = off.id;
    look.layers[0].stack.push_back(fx);
    look.layers[0].stack.push_back(off);
    look.links = {{layer_id, fx_id, 0}, {fx_id, off_id, 0}, {off_id, 0, 0}};
    {
        doc::AudioTrack at;
        at.id = rig.doc.next_effect_id++;
        doc::Placement ap;
        ap.id = rig.doc.next_effect_id++;
        ap.target = rig.look;
        at.placements.push_back(ap);
        rig.doc.root().audio.clear();
        rig.doc.root().audio.push_back(at);
    }

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    CHECK_EQ(doc::media_asset_frame(sources[0], 0.0), 0.0);
    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    if (voice.empty()) return;
    CHECK_EQ(doc::media_asset_frame(voice[0], 0.0), 0.0);
}

TEST(flatten_audio_only_asset_is_image_dormant) {
    // An asset with no picture (audio import without cover art: zero
    // frames and dimensions) emits NOTHING on the picture walk and no
    // compiler Source head; the audio walk still carries the voice,
    // unbounded (the block windows it).
    Rig rig(0);
    doc::Placement ap;
    ap.id = rig.doc.next_effect_id++;
    ap.target = rig.look;
    doc::AudioTrack at;
    at.id = rig.doc.next_effect_id++;
    at.placements.push_back(ap);
    rig.doc.root().audio.clear();
    rig.doc.root().audio.push_back(at);

    CHECK(doc::flatten_media_sources(rig.doc, rig.doc.root_sequence)
              .empty());
    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].asset, rig.asset);
    CHECK_EQ(voice[0].t_out, doc::kUnbounded);

    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 10);
    for (const gfx::GraphNode& n : g.nodes)
        CHECK(n.kind != gfx::GraphNode::Kind::Source);
}

TEST(flatten_timeline_lock_reads_the_root_clock) {
    // A locked media node ignores the composed placement map: every
    // block reads media = root + slip, so two placements of the look
    // share one stream key and identical positions.
    Rig rig(200);
    rig.placement().t_in = 40;
    rig.placement().source_in = 25;
    rig.placement().speed = 2.0f;
    rig.doc.looks[0].layers[0].timeline_lock = true;
    rig.doc.looks[0].layers[0].slip = 3;
    doc::Placement second;
    second.id = rig.doc.next_effect_id++;
    second.target = rig.look;
    second.t_in = 150;
    rig.doc.root().tracks[0].placements.push_back(second);

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{2});
    const MediaInstance& a = sources[0];
    const MediaInstance& b = sources[1];
    CHECK_EQ(a.speed, 1.0);
    CHECK_EQ(b.speed, 1.0);
    // Identity from the root: block timing shapes only the WINDOW.
    CHECK_EQ(doc::media_asset_frame(a, 50.0), 53.0);
    CHECK_EQ(doc::media_asset_frame(b, 160.0), 163.0);
    CHECK_EQ(a.t_in, 40.0);
    CHECK_EQ(b.t_in, 150.0);
    // One stream: the key drops the instance path.
    CHECK_EQ(a.key, b.key);

    // Compiler agreement at a frame where one block plays.
    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 50);
    int found = 0;
    for (const gfx::GraphNode& n : g.nodes)
        if (n.kind == gfx::GraphNode::Kind::Source) {
            ++found;
            CHECK_EQ(n.key, a.key);
        }
    CHECK_EQ(found, 1);
}

TEST(flatten_and_compiler_agree_frame_by_frame) {
    Rig rig(50);
    rig.doc.fps = 60.0;
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
    // A MISMATCHED-RATE asset (24 in 60): its conformed window must
    // close on the same fractional bound in both walks.
    doc::Asset slow;
    slow.id = rig.doc.next_effect_id++;
    slow.frame_count = 10;   // 25 clock frames at rate 0.4
    slow.fps = 24.0;
    rig.doc.assets.push_back(slow);
    doc::Look wrap;
    wrap.id = rig.doc.next_effect_id++;
    doc::Layer media;
    media.id = rig.doc.next_effect_id++;
    media.asset = slow.id;
    media.slip = 3;
    wrap.layers.push_back(std::move(media));
    const uint64_t wrap_id = wrap.id;
    rig.doc.looks.push_back(std::move(wrap));
    doc::SeqTrack lane2;
    lane2.id = rig.doc.next_effect_id++;
    doc::Placement third;
    third.id = rig.doc.next_effect_id++;
    third.target = wrap_id;
    third.t_in = 2;
    rig.doc.root().tracks.push_back(std::move(lane2));
    rig.doc.root().tracks[1].placements.push_back(third);

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    for (uint32_t f = 0; f < 60; f += 3) {
        std::vector<uint64_t> from_flatten;
        for (const MediaInstance& c : sources)
            if (doc::media_active(c, static_cast<double>(f)))
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
    doc::Layer media;
    media.id = rig.doc.next_effect_id++;
    media.asset = rig.asset;
    second.layers.push_back(std::move(media));
    const uint64_t second_id = second.id;
    rig.doc.looks.push_back(std::move(second));
    doc::Placement late;
    late.id = rig.doc.next_effect_id++;
    late.target = second_id;
    late.t_in = 10;
    rig.doc.root().tracks[0].placements.push_back(late);

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    int active = 0;
    for (const MediaInstance& c : sources)
        if (doc::media_active(c, 20.0)) ++active;
    CHECK_EQ(active, 2);

    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 20);
    int source_nodes = 0;
    uint64_t shown = 0;
    for (const gfx::GraphNode& n : g.nodes)
        if (n.kind == gfx::GraphNode::Kind::Source) {
            ++source_nodes;
            shown = g.instances[static_cast<size_t>(n.instance)].look;
        }
    CHECK_EQ(source_nodes, 1);
    CHECK_EQ(shown, second_id);
}

TEST(flatten_audio_reads_audio_tracks_only) {
    // Video lanes are silent - sound rides audio placements, and a look
    // target contributes its sources' PCM in lockstep through the map.
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
    rig.doc.root().audio.clear();
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

// Lays the standard audio path: one audio placement of `target` on the
// root sequence, unity gains.
static void lay_audio_block(Document& d, uint64_t target) {
    doc::AudioTrack at;
    at.id = d.next_effect_id++;
    doc::Placement ap;
    ap.id = d.next_effect_id++;
    ap.target = target;
    at.placements.push_back(ap);
    d.root().audio.clear();
    d.root().audio.push_back(at);
}

TEST(flatten_audio_voice_is_the_bottom_output_chain) {
    // One wire = one voice: the synthesized table fans every chain into
    // the Output, and the FIRST port-0 link - the bottom chain - wins.
    // The picture walk still emits both layers.
    Rig rig(60);
    doc::Asset b;
    b.id = rig.doc.next_effect_id++;
    b.frame_count = 60;
    rig.doc.assets.push_back(b);
    doc::Layer second;
    second.id = rig.doc.next_effect_id++;
    second.source = doc::LayerSourceKind::Media;
    second.asset = b.id;
    rig.doc.looks[0].layers.push_back(second);
    lay_audio_block(rig.doc, rig.look);

    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{2});
    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].asset, rig.asset);
    CHECK_EQ(voice[0].layer, rig.doc.looks[0].layers[0].id);
    CHECK_EQ(voice[0].op_count, uint32_t{0});
}

TEST(flatten_audio_voice_survives_a_preset_splice) {
    // Splicing a preset into the BOTTOM chain re-terminates it through
    // the group. Stacking order IS the link order, so the splice must
    // land IN PLACE (reconnect_command): the bottom chain stays the
    // bottom link and the voice stays on the media - append semantics
    // would flip it to the top layer (a silent generator).
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    const uint64_t media_layer = look.layers[0].id;
    doc::Layer top;
    top.id = d.next_effect_id++;
    top.source = doc::LayerSourceKind::Solid;
    look.layers.push_back(top);
    lay_audio_block(d, rig.look);

    doc::UndoStack undo;
    doc::Group g;
    g.id = d.next_effect_id++;
    std::vector<doc::EffectInstance> members;
    members.push_back(doc::make_effect(d, doc::EffectType::Grain));
    members.push_back(doc::make_effect(d, doc::EffectType::Posterize));
    const uint64_t m0 = members[0].id, m1 = members[1].id;
    undo.execute(d, doc::insert_group_command(rig.look, 0, g,
                                              std::move(members)));
    // The drop gesture's splice: the chain end re-terminates through
    // the group IN PLACE, then the feed wires into the group's head.
    undo.execute(d, doc::reconnect_command(rig.look, {media_layer, 0, 0},
                                           {m1, 0, 0}));
    undo.execute(d, doc::connect_command(rig.look, {media_layer, m0, 0}));

    const auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].asset, rig.asset);
    CHECK_EQ(voice[0].layer, media_layer);
    // The spliced chain kept the BOTTOM position of the Output fan-in.
    uint64_t bottom_out = 0;
    for (const doc::NodeLink& l : look.links)
        if (l.to == 0 && l.to_port == 0) {
            bottom_out = l.from;
            break;
        }
    CHECK_EQ(bottom_out, m1);

    // Undoing the whole splice restores the voice unchanged.
    while (undo.can_undo()) undo.undo(d);
    const auto voice2 = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice2.size(), size_t{1});
    CHECK_EQ(voice2[0].asset, rig.asset);
}

TEST(flatten_audio_collects_voice_ops_in_play_order) {
    // Audio-modifier hops collect source-first; video effects in the
    // same chain pass audio through; a bypassed hop drops out while the
    // chain keeps flowing through it.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    const uint64_t src_id = look.layers[0].id;
    look.layers[0].stack.push_back(
        doc::make_effect(d, doc::EffectType::AudioDelay));
    look.layers[0].stack.push_back(
        doc::make_effect(d, doc::EffectType::Posterize));
    look.layers[0].stack.push_back(
        doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t delay_id = look.layers[0].stack[0].id;
    const uint64_t poster_id = look.layers[0].stack[1].id;
    const uint64_t gain_id = look.layers[0].stack[2].id;
    look.layers[0].stack[2].params[0] = 0.5f;
    look.links = {{src_id, delay_id, 0},
                  {delay_id, poster_id, 0},
                  {poster_id, gain_id, 0},
                  {gain_id, 0, 0}};
    lay_audio_block(d, rig.look);

    auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].op_count, uint32_t{2});
    CHECK(voice[0].ops[0].type == doc::EffectType::AudioDelay);
    CHECK(voice[0].ops[1].type == doc::EffectType::AudioGain);
    CHECK_EQ(voice[0].ops[1].params[0], 0.5f);

    look.layers[0].stack[0].bypass = true;
    voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice[0].op_count, uint32_t{1});
    CHECK(voice[0].ops[0].type == doc::EffectType::AudioGain);
}

TEST(flatten_audio_split_output_reads_its_own_port) {
    // SPLIT: the Output's dedicated audio-in (port 1) is the voice;
    // unwired means SILENT, never a fallback onto the image chain.
    Rig rig(60);
    Document& d = rig.doc;
    lay_audio_block(d, rig.look);

    CHECK_EQ(doc::flatten_audio_sources(d, d.root_sequence).size(),
             size_t{1});
    d.looks[0].audio_split = true;
    CHECK_EQ(doc::flatten_audio_sources(d, d.root_sequence).size(),
             size_t{0});
    doc::ensure_links(d.looks[0]);
    d.looks[0].links.push_back({d.looks[0].layers[0].id, 0, 1});
    CHECK_EQ(doc::flatten_audio_sources(d, d.root_sequence).size(),
             size_t{1});
}

TEST(flatten_audio_nested_voice_composes_ops) {
    // Inner hops run first, the enclosing look's append after - the DSP
    // list composes through nesting the way the clocks do.
    Rig rig(60);
    Document& d = rig.doc;
    d.looks[0].layers[0].stack.push_back(
        doc::make_effect(d, doc::EffectType::AudioDelay));

    doc::Look outer;
    outer.id = d.next_effect_id++;
    outer.name = "outer";
    doc::Layer ref;
    ref.id = d.next_effect_id++;
    ref.source = doc::LayerSourceKind::LookRef;
    ref.target = rig.look;
    ref.stack.push_back(doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t gain_id = ref.stack[0].id;
    const uint64_t ref_id = ref.id;
    outer.layers.push_back(std::move(ref));
    outer.links = {{ref_id, gain_id, 0}, {gain_id, 0, 0}};
    d.looks.push_back(std::move(outer));
    lay_audio_block(d, d.looks.back().id);

    const auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].asset, rig.asset);
    CHECK_EQ(voice[0].op_count, uint32_t{2});
    CHECK(voice[0].ops[0].type == doc::EffectType::AudioDelay);
    CHECK(voice[0].ops[1].type == doc::EffectType::AudioGain);
}

TEST(resolve_audio_chain_composes_to_closed_form) {
    // The audio chain FEEDING a node: the wired hop itself counts,
    // video effects pass through, nested refs resolve through their
    // own Output with inner hops first; a sequence ref has no single
    // voice and reads silent.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    look.layers[0].slip = 3;
    look.layers[0].stack.push_back(
        doc::make_effect(d, doc::EffectType::AudioDelay));
    const uint64_t delay_id = look.layers[0].stack[0].id;

    // Synthesized chain media -> delay -> Output: from the delay the
    // chain includes it; from the layer it is the raw media.
    const doc::AudioChain from_delay =
        doc::resolve_audio_chain(d, look, delay_id);
    CHECK_EQ(from_delay.asset, rig.asset);
    CHECK_EQ(from_delay.slip, uint32_t{3});
    CHECK_EQ(from_delay.op_count, uint32_t{1});
    CHECK(from_delay.ops[0].type == doc::EffectType::AudioDelay);
    const doc::AudioChain from_media =
        doc::resolve_audio_chain(d, look, look.layers[0].id);
    CHECK_EQ(from_media.asset, rig.asset);
    CHECK_EQ(from_media.op_count, uint32_t{0});

    doc::Look outer;
    outer.id = d.next_effect_id++;
    doc::Layer ref;
    ref.id = d.next_effect_id++;
    ref.source = doc::LayerSourceKind::LookRef;
    ref.target = rig.look;
    ref.stack.push_back(doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t gain_id = ref.stack[0].id;
    const uint64_t ref_id = ref.id;
    outer.layers.push_back(std::move(ref));
    outer.links = {{ref_id, gain_id, 0}, {gain_id, 0, 0}};
    d.looks.push_back(std::move(outer));
    const doc::AudioChain nested =
        doc::resolve_audio_chain(d, d.looks.back(), gain_id);
    CHECK_EQ(nested.asset, rig.asset);
    CHECK_EQ(nested.slip, uint32_t{3});
    CHECK_EQ(nested.op_count, uint32_t{2});
    CHECK(nested.ops[0].type == doc::EffectType::AudioDelay);
    CHECK(nested.ops[1].type == doc::EffectType::AudioGain);

    doc::Layer sref;
    sref.id = d.next_effect_id++;
    sref.source = doc::LayerSourceKind::SequenceRef;
    sref.target = d.root_sequence;
    d.looks.back().layers.push_back(sref);
    CHECK_EQ(doc::resolve_audio_chain(d, d.looks.back(), sref.id).asset,
             uint64_t{0});
}

TEST(flatten_skips_hidden_and_dangling_sources) {
    Rig rig(50);
    rig.doc.looks[0].layers[0].visible = false;
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
    rig.doc.looks[0].layers[0].visible = true;
    rig.placement().target = 999999;   // dangling block: dormant
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
    rig.placement().target = rig.look;
    // Dangling ASSET id (media removed): dormant like an unbound node
    // (emit_media guards both walks).
    rig.doc.looks[0].layers[0].asset = 888888;
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
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

TEST(flatten_conforms_mismatched_media_rate) {
    // 30fps media in a 60fps project: the media frame advances at half
    // the clock (1x in TIME, each frame held twice), slip stays
    // media-frame-exact past the rate, and derived lengths double.
    Rig rig(100);
    rig.doc.fps = 60.0;
    rig.doc.assets[0].fps = 30.0;
    rig.doc.looks[0].layers[0].slip = 10;

    // 90 media frames past the slip = 180 clock frames.
    CHECK_EQ(doc::layer_source_length(rig.doc, rig.doc.looks[0].layers[0],
                                      60.0),
             uint32_t{180});
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{180});

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    const MediaInstance& c = sources[0];
    CHECK_EQ(c.rate, 0.5);
    CHECK_EQ(c.shift, int64_t{10});
    CHECK_EQ(c.t_in, 0.0);
    CHECK_EQ(c.t_out, 180.0);
    CHECK_EQ(doc::media_asset_frame(c, 0.0), 10.0);
    CHECK_EQ(doc::media_asset_frame(c, 1.0), 10.0);
    CHECK_EQ(doc::media_asset_frame(c, 2.0), 11.0);
    CHECK_EQ(doc::media_asset_frame(c, 179.0), 99.0);
    CHECK(!doc::media_active(c, 180.0));

    // The compiler windows the head on the same conformed bounds.
    const gfx::RenderGraph g_on =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 179);
    bool has_src = false;
    for (const gfx::GraphNode& n : g_on.nodes)
        if (n.kind == gfx::GraphNode::Kind::Source) has_src = true;
    CHECK(has_src);
    const gfx::RenderGraph g_off =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 180);
    for (const gfx::GraphNode& n : g_off.nodes)
        CHECK(n.kind != gfx::GraphNode::Kind::Source);

    // The AUDIO walk carries the same clock-domain affine (the mix maps
    // by seconds and must not double-conform) with the media-frame
    // shift held on the instance, not folded in.
    doc::AudioTrack at;
    at.id = rig.doc.next_effect_id++;
    doc::Placement ap;
    ap.id = rig.doc.next_effect_id++;
    ap.target = rig.look;
    at.placements.push_back(ap);
    rig.doc.root().audio.clear();
    rig.doc.root().audio.push_back(at);
    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].rate, 0.5);
    CHECK_EQ(voice[0].shift, int64_t{10});
    CHECK_EQ(doc::media_source_frame(voice[0], 40.0), 40.0);
}

TEST(flatten_conform_razor_identity) {
    // Cutting a conformed block and butting the halves is bit-identical:
    // the right half's trimmed head lands on the same media frames the
    // whole block showed, through the same stream key.
    Rig rig(100);
    rig.doc.fps = 60.0;
    rig.doc.assets[0].fps = 30.0;
    doc::Placement& left = rig.placement();
    left.t_out = 91;
    doc::Placement right;
    right.id = rig.doc.next_effect_id++;
    right.target = rig.look;
    right.t_in = 91;
    right.source_in = 91;   // trim_placement_head's truncate rule
    rig.doc.root().tracks[0].placements.push_back(right);

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{2});
    const MediaInstance& a = sources[0];
    const MediaInstance& b = sources[1];
    CHECK_EQ(a.key, b.key);   // razored halves share one stream
    for (uint32_t f = 91; f < 97; ++f)
        CHECK_EQ(doc::media_asset_frame(b, static_cast<double>(f)),
                 std::floor(static_cast<double>(f) * 0.5));
}

TEST(pinned_fps_look_conforms_through_the_hop) {
    // A 30fps-PINNED look in a 60fps project: its own clock ticks 30
    // (media at 30fps maps 1:1 inside it), the hop ratio rides the
    // affine, and derived spans double on the parent timeline.
    Rig rig(90);
    rig.doc.fps = 60.0;
    rig.doc.assets[0].fps = 30.0;
    rig.doc.looks[0].format.fps = 30.0;

    CHECK_EQ(doc::look_duration(rig.doc, rig.doc.looks[0]), uint32_t{90});
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{180});

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    const MediaInstance& c = sources[0];
    CHECK_EQ(c.rate, 1.0);    // media matches ITS look's pinned clock
    CHECK_EQ(c.speed, 0.5);   // the hop ratio rides the affine
    CHECK_EQ(c.t_out, 180.0);
    CHECK_EQ(doc::media_asset_frame(c, 3.0), 1.0);
    CHECK_EQ(doc::media_asset_frame(c, 179.0), 89.0);

    // The compiler's nested instance ticks the PINNED clock: at root
    // frame 100 the look's local frame is 50 - lanes, value graph and
    // stateful effects all clock on it.
    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 100);
    bool found = false;
    for (const gfx::LookInstance& li : g.instances)
        if (li.look == rig.look) {
            found = true;
            CHECK_EQ(li.local_frame, uint32_t{50});
        }
    CHECK(found);

    // Razor identity across the pinned hop: a cut on a child-frame
    // boundary reproduces the uncut media frames exactly (an off-grid
    // cut truncates source_in - the same ONE rounding rule fractional
    // speeds follow).
    doc::UndoStack undo;
    if (auto cut = doc::razor_track_command(rig.doc, rig.doc.root_sequence,
                                            rig.lane, 62))
        undo.execute(rig.doc, std::move(cut));
    const auto halves =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(halves.size(), size_t{2});
    if (halves.size() < 2) return;
    CHECK_EQ(halves[0].key, halves[1].key);
    for (uint32_t f = 62; f < 68; ++f)
        CHECK_EQ(doc::media_asset_frame(halves[1],
                                        static_cast<double>(f)),
                 std::floor(static_cast<double>(f) * 0.5));
}

TEST(still_assets_never_conform) {
    // A true still's frame count is authored on the PROJECT clock (the
    // duration entry), so the conform rate must stay 1 even though the
    // mezzanine stamps its synthetic 30fps.
    Rig rig(150);
    rig.doc.fps = 60.0;
    rig.doc.assets[0].fps = 30.0;
    rig.doc.assets[0].still = true;
    CHECK_EQ(doc::media_conform_rate(rig.doc, rig.doc.assets[0], 60.0),
             1.0);
    CHECK_EQ(doc::layer_source_length(rig.doc, rig.doc.looks[0].layers[0],
                                      60.0),
             uint32_t{150});
    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    CHECK_EQ(sources[0].rate, 1.0);
    CHECK_EQ(sources[0].t_out, 150.0);
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
    rig.doc.root().audio.clear();
    rig.doc.root().audio.push_back(at);
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{190});
    // An explicit duration overrides the derivation.
    rig.doc.root().duration = 42;
    CHECK_EQ(doc::sequence_duration(rig.doc, rig.doc.root()),
             uint32_t{42});
}

TEST(flatten_hidden_lane_leaves_the_video_walk) {
    // A hidden lane's blocks leave the video flatten entirely (the
    // compiler skips them identically, so the pool never prewarms a
    // ghost). The AUDIO walk ignores hidden - video lanes carry no
    // sound either way.
    Rig rig(100);
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{1});
    rig.doc.root().tracks[0].hidden = true;
    CHECK(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).empty());
}
