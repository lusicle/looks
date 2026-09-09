#include "doc/instances.h"

#include <algorithm>
#include <cmath>

#include "doc/command.h"
#include "doc/effects.h"
#include "doc/group_commands.h"
#include "doc/layer_commands.h"
#include "doc/look_commands.h"
#include "doc/stack_commands.h"
#include "doc/serialize.h"
#include "media/decode_pool.h"
#include "gfx/graph.h"
#include "doc_fixture.h"
#include "test_framework.h"
#include "util/hash.h"

using namespace looks;
using doc::MediaInstance;
using doc::Document;

namespace {

struct Rig {
    Document doc = doc_with_look();
    uint64_t asset = 0;
    uint64_t look = 0;
    uint64_t lane = 0;

    explicit Rig(uint32_t frames = 100) {
        doc::Asset a;
        a.id = doc.next_effect_id++;
        a.frame_count = frames;
        doc.assets.push_back(a);
        asset = a.id;
        doc.looks[0].sources[0].asset = asset;
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
    const uint64_t path = hash_combine(
        hash_combine(rig.doc.root_sequence, rig.lane), rig.look);
    const uint64_t layer_id = rig.doc.looks[0].sources[0].id;
    CHECK_EQ(c.key,
             hash_combine(hash_combine(path, layer_id), rig.asset));
}

TEST(track_inputs_follow_video_wires_through_nested_split_outputs) {
    Rig rig;
    auto& look = rig.doc.looks[0];
    const auto look_id = look.id;
    auto audio = doc::make_source(rig.doc, doc::SourceKind::Media);
    audio.asset = 999;
    look.sources.push_back(audio);
    look.audio_split = true;
    look.links.push_back({audio.id, 0, 1});
    auto outer = doc::make_look(rig.doc, "tracking");
    auto nested = doc::make_source(rig.doc, doc::SourceKind::LookRef);
    nested.target = look_id;
    auto pin = doc::make_effect(rig.doc, doc::EffectType::TrackPin);
    outer.sources = {nested};
    outer.effects = {pin};
    outer.links = {{nested.id, pin.id, 0}, {pin.id, 0, 0}};
    const auto outer_id = outer.id;
    rig.doc.looks.push_back(outer);
    CHECK(doc::upstream_video_assets(rig.doc, outer_id, pin.id) == std::vector<uint64_t>{rig.asset});
    const auto graph = gfx::compile_graph(rig.doc, outer_id, 20);
    CHECK(graph.valid);
    bool found = false;
    for (const auto& node : graph.nodes)
        if (node.kind == gfx::GraphNode::Kind::Effect && node.effect_index == 0 &&
            graph.instances[node.instance].look == outer_id) {
            CHECK_EQ(node.media_asset, rig.asset);
            CHECK_EQ(node.media_frame, 20.0);
            found = true;
        }
    CHECK(found);
}

TEST(slideshow_bins_sort_and_serialization) {
    auto d = doc_with_look();
    d.fps = 30;
    d.bins = {{30, "selected", 0}, {31, "child", 30}, {32, "other", 0}};
    auto& layer = d.looks[0].sources[0];
    layer.source = doc::SourceKind::Slideshow;
    layer.slide_bin = 30;
    layer.slide_seconds = 2;
    layer.slide_speed = 2;
    for (int i = 0; i < 4; ++i) {
        doc::Asset a;
        a.id = 100 + i;
        a.name = std::string(1, static_cast<char>('d' - i));
        a.width = 100 + i * 100;
        a.height = 100;
        a.byte_size = 400 - i * 100;
        a.frame_count = 60 + i * 30;
        a.fps = 30;
        a.bin = i == 0 ? 30 : i == 1 ? 31 : 32;
        d.assets.push_back(a);
    }
    doc::Asset audio = d.assets[0];
    audio.id = 200;
    audio.path = "cover.MP3";
    d.assets.push_back(audio);
    CHECK_EQ(doc::slideshow_assets(d, layer).size(), size_t{2});
    CHECK_EQ(doc::slideshow_assets(d, layer)[0]->id, uint64_t{101});
    CHECK_EQ(doc::layer_source_length(d, layer, 30), 60u);
    layer.slide_subbins = false;
    CHECK_EQ(doc::slideshow_assets(d, layer).size(), size_t{1});
    layer.slide_subbins = true;
    layer.slide_order = 1;
    CHECK_EQ(doc::slideshow_assets(d, layer)[0]->id, uint64_t{101});
    layer.slide_order = 2;
    CHECK_EQ(doc::slideshow_assets(d, layer)[0]->id, uint64_t{100});
    layer.slide_order = 3;
    CHECK_EQ(doc::slideshow_assets(d, layer)[0]->id, uint64_t{100});
    layer.slide_reverse = true;
    CHECK_EQ(doc::slideshow_assets(d, layer)[0]->id, uint64_t{101});
    layer.slide_order = 4;
    layer.slide_seed = 43;
    const uint64_t shuffled = doc::slideshow_assets(d, layer)[0]->id;
    std::reverse(d.assets.begin(), d.assets.end());
    CHECK_EQ(doc::slideshow_assets(d, layer)[0]->id, shuffled);
    layer.slide_fade = 0.4f;
    layer.slide_fit = 2;
    layer.slide_video_loop = true;
    layer.slide_end = 1;
    const auto json = doc::doc_to_json(d);
    const Document loaded = doc::doc_from_json(json);
    const auto& copy = loaded.looks[0].sources[0];
    CHECK(copy.source == doc::SourceKind::Slideshow);
    CHECK_EQ(copy.slide_bin, layer.slide_bin);
    CHECK_EQ(copy.slide_fit, layer.slide_fit);
    CHECK_EQ(copy.slide_order, layer.slide_order);
    CHECK_EQ(copy.slide_seed, layer.slide_seed);
    CHECK_EQ(copy.slide_seconds, layer.slide_seconds);
    CHECK_EQ(copy.slide_speed, layer.slide_speed);
    CHECK_EQ(copy.slide_fade, layer.slide_fade);
    CHECK(copy.slide_reverse && copy.slide_video_loop);
    CHECK_EQ(copy.slide_end, layer.slide_end);
    CHECK_EQ(loaded.assets[0].byte_size, d.assets[0].byte_size);
}

TEST(slideshow_decode_plan_matches_graph_through_time_maps) {
    Rig rig;
    rig.doc.fps = 30;
    auto& look = rig.doc.looks[0];
    auto& layer = look.sources[0];
    layer.source = doc::SourceKind::Slideshow;
    layer.slide_seconds = 1;
    layer.slide_fade = 0.2f;
    auto& first = rig.doc.assets[0];
    first.width = 160;
    first.height = 120;
    first.name = "a";
    first.still = true;
    doc::Asset second = first;
    second.id = rig.doc.next_effect_id++;
    second.name = "b";
    second.still = false;
    second.fps = 24;
    rig.doc.assets.push_back(second);
    std::vector<media::AssetBundle> bundles;
    for (const auto& a : rig.doc.assets) {
        media::AssetBundle bundle;
        bundle.asset = a.id;
        bundle.frames = a.frame_count;
        bundle.mez = "unused.mez";
        bundles.push_back(bundle);
    }
    rig.placement().t_in = 10;
    rig.placement().t_out = 400;
    rig.placement().source_in = 12;
    rig.placement().speed = 0.5f;
    media::DecodePool pool("slideshow-test");
    uint64_t revision = 0;
    for (uint32_t end = 0; end < 3; ++end) {
        layer.slide_end = end;
        for (float speed : {0.5f, 2.0f}) {
            layer.slide_speed = speed;
            pool.set_document(rig.doc, rig.doc.root_sequence, bundles, ++revision);
            for (uint32_t frame : {0u, 10u, 46u, 50u, 70u, 82u, 130u, 250u, 399u, 400u}) {
                const auto requests = pool.plan(frame);
                const auto graph = gfx::compile_graph(rig.doc, rig.doc.root_sequence, frame);
                CHECK(graph.valid);
                std::vector<uint64_t> keys;
                for (const auto& node : graph.nodes)
                    if (node.kind == gfx::GraphNode::Kind::Source) keys.push_back(node.key);
                CHECK_EQ(keys.size(), requests.size());
                for (const auto& request : requests)
                    CHECK(std::find(keys.begin(), keys.end(), request.key) != keys.end());
            }
        }
    }
}

TEST(slideshow_video_frames_and_crossfade) {
    MediaInstance c;
    c.t_out = 1000;
    c.slide_count = 2;
    c.slide_index = 1;
    c.slide_period = 30;
    c.slide_fade = 6;
    c.rate = 24.0 / 30.0;
    CHECK(!doc::media_active(c, 29));
    CHECK(doc::media_active(c, 30));
    CHECK_EQ(doc::media_asset_frame(c, 40), 8.0);
    CHECK(doc::media_active(c, 63));
    CHECK_EQ(doc::media_asset_frame(c, 63), 23.0);
    CHECK(!doc::media_active(c, 66));
    c.slide_end = 1;
    CHECK(doc::media_active(c, 600));
    c.slide_end = 2;
    CHECK(!doc::media_active(c, 60));
    c.slide_end = 0;
    c.repeat_frames = 6;
    CHECK_EQ(doc::media_asset_frame(c, 40), 2.0);
    const auto fade = doc::slideshow_sample(33, 30, 2, 6, 0);
    CHECK_EQ(fade.current, size_t{1});
    CHECK_EQ(fade.previous, size_t{0});
    CHECK_EQ(fade.mix, 0.5f);
}

TEST(slideshow_large_bin_keeps_other_source_streams) {
    auto d = doc_with_look();
    auto& look = d.looks[0];
    look.sources[0].source = doc::SourceKind::Slideshow;
    for (uint64_t i = 0; i < 1100; ++i) {
        doc::Asset asset;
        asset.id = d.next_effect_id++;
        asset.width = asset.height = 64;
        asset.frame_count = 30;
        asset.still = true;
        d.assets.push_back(asset);
    }
    doc::Source media;
    media.id = d.next_effect_id++;
    media.asset = d.assets[0].id;
    look.sources.push_back(media);
    const auto sources = doc::flatten_media_sources(d, look.id);
    CHECK_EQ(sources.size(), size_t{1101});
    CHECK_EQ(sources.back().layer, media.id);
    int active = 0;
    for (const auto& source : sources) if (doc::media_active(source, 0)) ++active;
    CHECK_EQ(active, 2);
}

TEST(slideshow_fit_modes) {
    float rect[4];
    gfx::source_fit_rect(100, 200, 400, 300, rect, 0);
    CHECK_EQ(rect[0], 125.0f);
    CHECK_EQ(rect[2], 150.0f);
    CHECK_EQ(rect[3], 300.0f);
    gfx::source_fit_rect(100, 200, 400, 300, rect, 1);
    CHECK_EQ(rect[1], -250.0f);
    CHECK_EQ(rect[2], 400.0f);
    CHECK_EQ(rect[3], 800.0f);
    gfx::source_fit_rect(100, 200, 400, 300, rect, 2);
    CHECK_EQ(rect[0], 0.0f);
    CHECK_EQ(rect[1], 0.0f);
    CHECK_EQ(rect[2], 400.0f);
    CHECK_EQ(rect[3], 300.0f);
}

TEST(flatten_composes_block_map_and_slip_in_closed_form) {
    Rig rig(100);
    rig.placement().t_in = 10;
    rig.placement().source_in = 4;
    rig.placement().speed = 2.0f;
    rig.doc.looks[0].sources[0].slip = 6;

    const auto sources =
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(sources.size(), size_t{1});
    const MediaInstance& c = sources[0];
    // Slip applies after the rate step.
    CHECK_EQ(c.t_in, 10.0);
    CHECK_EQ(doc::media_asset_frame(c, 10.0), 10.0);
    CHECK_EQ(doc::media_asset_frame(c, 20.0), 30.0);
    // 90 local frames at 2x speed from t_in 10 end at root 55.
    CHECK_EQ(c.t_out, 55.0);
    CHECK(doc::media_active(c, 54.9));
    CHECK(!doc::media_active(c, 55.0));
}

TEST(flatten_offset_shim_emits_a_shifted_stream) {
    Rig rig(100);
    doc::Look& look = rig.doc.looks[0];
    const uint64_t layer_id = look.sources[0].id;
    doc::EffectInstance off =
        doc::make_effect(rig.doc, doc::EffectType::Offset);
    off.params[0] = 10.0f;   // +10 frames
    off.params[1] = 2.0f;    // 2 = shift video and audio
    const uint64_t off_id = off.id;
    look.effects.push_back(off);
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
    CHECK_EQ(sh.t_out, 90.0);   // 100 frames minus the 10 frame shift
    CHECK(sh.key != base.key);

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

    // Closed gate: the output goes black while the unused base stays active.
    CHECK(!doc::media_active(sh, 95.0));
    CHECK(doc::media_active(base, 95.0));
    const gfx::RenderGraph g2 =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 95);
    CHECK_EQ(g2.nodes[static_cast<size_t>(g2.output)].kind,
             gfx::GraphNode::Kind::Generator);

    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    if (voice.empty()) return;
    CHECK_EQ(doc::media_asset_frame(voice[0], 0.0), 10.0);

    const doc::AudioChain chain =
        doc::resolve_audio_chain(rig.doc, look, off_id);
    CHECK_EQ(chain.asset, rig.asset);
    CHECK_EQ(chain.offset, int64_t{10});
    CHECK(!chain.locked);
    look.sources[0].timeline_lock = true;
    CHECK(doc::resolve_audio_chain(rig.doc, look, off_id).locked);
}

TEST(solo_has_the_same_scope_for_audio_offset_and_video) {
    Rig rig(100);
    auto& look = rig.doc.looks[0];
    auto offset = doc::make_effect(rig.doc, doc::EffectType::Offset);
    offset.params[0] = 10;
    offset.params[1] = 2;
    auto gain = doc::make_effect(rig.doc, doc::EffectType::AudioGain);
    auto video = doc::make_effect(rig.doc, doc::EffectType::Invert);
    video.solo = true;
    look.effects = {offset, gain, video};
    look.links = {{look.sources[0].id, offset.id, 0}, {offset.id, gain.id, 0},
        {gain.id, video.id, 0}, {video.id, 0, 0}};
    auto chain = doc::resolve_audio_chain(rig.doc, look, video.id);
    CHECK_EQ(chain.offset, int64_t{0});
    CHECK_EQ(chain.op_count, uint32_t{0});
    auto program = doc::flatten_audio_program(rig.doc, look.id);
    for (const auto& n : program.nodes) CHECK(!n.has_op);
    look.effects[0].solo = true;
    look.effects[1].solo = true;
    chain = doc::resolve_audio_chain(rig.doc, look, video.id);
    CHECK_EQ(chain.offset, int64_t{10});
    CHECK_EQ(chain.op_count, uint32_t{1});
    program = doc::flatten_audio_program(rig.doc, look.id);
    CHECK(std::any_of(program.nodes.begin(), program.nodes.end(),
        [&](const doc::AudioNode& n) { return n.has_op && n.doc_id == gain.id; }));
    auto group = doc::make_group(rig.doc, "bypassed solo");
    group.bypass = true;
    look.groups.push_back(group);
    look.effects[2].group_id = group.id;
    look.effects[0].solo = look.effects[1].solo = false;
    chain = doc::resolve_audio_chain(rig.doc, look, video.id);
    CHECK_EQ(chain.offset, int64_t{10});
    CHECK_EQ(chain.op_count, uint32_t{1});
}

TEST(flatten_offset_shim_off_the_source_is_inert) {
    // The shim applies only when it sits directly on a source node.
    Rig rig(100);
    doc::Look& look = rig.doc.looks[0];
    const uint64_t layer_id = look.sources[0].id;
    doc::EffectInstance fx =
        doc::make_effect(rig.doc, doc::EffectType::Vignette);
    doc::EffectInstance off =
        doc::make_effect(rig.doc, doc::EffectType::Offset);
    off.params[0] = 10.0f;
    off.params[1] = 2.0f;
    const uint64_t fx_id = fx.id;
    const uint64_t off_id = off.id;
    look.effects.push_back(fx);
    look.effects.push_back(off);
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
    // An asset with no picture emits nothing on the picture walk.
    // The audio walk still carries the voice, unbounded.
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

TEST(entity_has_image_follows_the_wiring_not_the_layers) {
    using looks::doc::Source;
    using looks::doc::SourceKind;

    // Media with pixels, wired to the Output's video port: an image.
    Rig rig(100);
    rig.doc.looks[0].links.push_back({rig.doc.looks[0].sources[0].id, 0, 0});
    CHECK(doc::entity_has_image(rig.doc, rig.look));

    // The same wiring with an asset that has no picture: no image. This is
    // the combined routing, where sound rides the video wire.
    Rig audio(0);
    audio.doc.looks[0].links.push_back(
        {audio.doc.looks[0].sources[0].id, 0, 0});
    CHECK(!doc::entity_has_image(audio.doc, audio.look));

    // Split routing sends the voice to port 1, so port 0 stays unwired.
    Rig split(0);
    split.doc.looks[0].audio_split = true;
    split.doc.looks[0].links.push_back(
        {split.doc.looks[0].sources[0].id, 0, 1});
    CHECK(!doc::entity_has_image(split.doc, split.look));

    // A drawable layer that reaches nothing still shows nothing.
    Rig unwired(100);
    unwired.doc.looks[0].links.clear();
    CHECK(!doc::entity_has_image(unwired.doc, unwired.look));

    // A generator always draws, and the walk crosses an effect chain.
    Document gen = doc_with_look();
    gen.looks[0].sources[0].source = SourceKind::Gradient;
    gen.looks[0].effects.push_back(
        doc::make_effect(gen, doc::EffectType::Vignette));
    const uint64_t fx = gen.looks[0].effects[0].id;
    gen.looks[0].links.push_back({gen.looks[0].sources[0].id, fx, 0});
    gen.looks[0].links.push_back({fx, 0, 0});
    CHECK(doc::entity_has_image(gen, gen.looks[0].id));
    // A sequence reports what its video lanes carry.
    CHECK(!doc::entity_has_image(audio.doc, audio.doc.root_sequence));
    CHECK(doc::entity_has_image(rig.doc, rig.doc.root_sequence));
}

TEST(flatten_timeline_lock_reads_the_root_clock) {
    // A locked media node reads media = root + slip, so blocks share a key.
    Rig rig(200);
    rig.placement().t_in = 40;
    rig.placement().source_in = 25;
    rig.placement().speed = 2.0f;
    rig.doc.looks[0].sources[0].timeline_lock = true;
    rig.doc.looks[0].sources[0].slip = 3;
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
    doc::Placement second;
    second.id = rig.doc.next_effect_id++;
    second.target = rig.look;
    second.t_in = 40;
    second.source_in = 10;
    rig.doc.root().tracks[0].placements.push_back(second);
    // A 24 fps asset in a 60 fps project must conform alike in both walks.
    doc::Asset slow;
    slow.id = rig.doc.next_effect_id++;
    slow.frame_count = 10;   // 25 clock frames at rate 0.4
    slow.fps = 24.0;
    rig.doc.assets.push_back(slow);
    doc::Look wrap;
    wrap.id = rig.doc.next_effect_id++;
    doc::Source media;
    media.id = rig.doc.next_effect_id++;
    media.asset = slow.id;
    media.slip = 3;
    wrap.sources.push_back(std::move(media));
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
        // Two blocks of one look share a stream, so duplicate keys collapse.
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
    // On an overlap the flatten emits both and the compiler shows the later.
    Rig rig(100);
    doc::Look second;
    second.id = rig.doc.next_effect_id++;
    doc::Source media;
    media.id = rig.doc.next_effect_id++;
    media.asset = rig.asset;
    second.sources.push_back(std::move(media));
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

TEST(flatten_audio_sums_every_output_chain) {
    // The synthesized table fans every live chain into the Output.
    Rig rig(60);
    doc::Asset b;
    b.id = rig.doc.next_effect_id++;
    b.frame_count = 60;
    rig.doc.assets.push_back(b);
    doc::Source second;
    second.id = rig.doc.next_effect_id++;
    second.source = doc::SourceKind::Media;
    second.asset = b.id;
    rig.doc.looks[0].sources.push_back(second);
    rig.doc.looks[0].links.push_back({second.id, 0, 0});
    lay_audio_block(rig.doc, rig.look);

    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{2});
    const auto voice =
        doc::flatten_audio_sources(rig.doc, rig.doc.root_sequence);
    CHECK_EQ(voice.size(), size_t{2});
    if (voice.size() < 2) return;
    CHECK_EQ(voice[0].asset, rig.asset);
    CHECK_EQ(voice[0].layer, rig.doc.looks[0].sources[0].id);
    CHECK_EQ(voice[1].asset, b.id);
    CHECK_EQ(voice[1].layer, second.id);
}

TEST(flatten_audio_fan_in_sums_at_the_op_node) {
    // An op with a fan-in processes the summed signal wired to it.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    const uint64_t src_a = look.sources[0].id;
    doc::Asset b;
    b.id = d.next_effect_id++;
    b.frame_count = 60;
    d.assets.push_back(b);
    doc::Source second;
    second.id = d.next_effect_id++;
    second.source = doc::SourceKind::Media;
    second.asset = b.id;
    const uint64_t src_b = second.id;
    look.sources.push_back(second);
    look.effects.push_back(
        doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t gain_id = look.effects[0].id;
    look.links = {{src_a, gain_id, 0}, {src_b, gain_id, 0},
                  {gain_id, 0, 0}};
    lay_audio_block(d, rig.look);

    doc::AudioProgram prog = doc::flatten_audio_program(d, d.root_sequence);
    CHECK(prog.root >= 0);
    if (prog.root < 0) return;
    // Root is the placement hop over the look's program.
    const doc::AudioNode& hop = prog.nodes[prog.root];
    CHECK(hop.windowed);
    CHECK_EQ(hop.inputs.size(), size_t{1});
    const doc::AudioNode& op = prog.nodes[hop.inputs[0]];
    CHECK(op.has_op);
    CHECK(op.op.type == doc::EffectType::AudioGain);
    CHECK_EQ(op.inputs.size(), size_t{2});
    CHECK_EQ(prog.nodes[op.inputs[0]].asset, rig.asset);
    CHECK_EQ(prog.nodes[op.inputs[1]].asset, b.id);

    // The leaf view lists both, bottom first.
    const auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{2});
    if (voice.size() < 2) return;
    CHECK_EQ(voice[0].layer, src_a);
    CHECK_EQ(voice[1].layer, src_b);

    look.links = {{src_a, gain_id, 0}, {gain_id, 0, 0}, {src_b, 0, 0}};
    prog = doc::flatten_audio_program(d, d.root_sequence);
    CHECK(prog.root >= 0);
    if (prog.root < 0) return;
    const doc::AudioNode& hop2 = prog.nodes[prog.root];
    CHECK_EQ(hop2.inputs.size(), size_t{1});
    const doc::AudioNode& sum = prog.nodes[hop2.inputs[0]];
    CHECK(!sum.has_op);
    CHECK_EQ(sum.inputs.size(), size_t{2});
    const doc::AudioNode& gained = prog.nodes[sum.inputs[0]];
    CHECK(gained.has_op);
    CHECK_EQ(gained.inputs.size(), size_t{1});
    CHECK_EQ(prog.nodes[gained.inputs[0]].asset, rig.asset);
    CHECK_EQ(prog.nodes[sum.inputs[1]].asset, b.id);
}

TEST(flatten_audio_voice_skips_dangling_feeds) {
    // A dead wire at the bottom of a fan-in must not swallow the voice.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    const uint64_t media_layer = look.sources[0].id;
    lay_audio_block(d, rig.look);
    look.links.insert(look.links.begin(), {99991, 0, 0});
    auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].layer, media_layer);

    // In a group input fan-in the hop must pick the live producer.
    doc::UndoStack undo;
    look.effects.push_back(
        doc::make_effect(d, doc::EffectType::Grain));
    const uint64_t fx0 = look.effects[0].id;
    look.links.clear();
    look.links.push_back({media_layer, fx0, 0});
    look.links.push_back({fx0, 0, 0});
    doc::Group g = doc::make_group(d, "wrap");
    const uint64_t gid = g.id;
    undo.execute(d, doc::group_effects_command(rig.look, g, {d.look(rig.look).effects[0].id}));
    const doc::Group* placed = doc::find_group(look, gid);
    CHECK(placed && !placed->inputs.empty());
    const uint64_t slot0 = placed->inputs.front();
    look.links.insert(look.links.begin(), {99992, slot0, 0});
    voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].layer, media_layer);
}

TEST(flatten_audio_voice_survives_a_preset_splice) {
    // A splice lands in place, so the chain keeps its fan-in position.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    const uint64_t media_layer = look.sources[0].id;
    doc::Source top;
    top.id = d.next_effect_id++;
    top.source = doc::SourceKind::Solid;
    look.sources.push_back(top);
    lay_audio_block(d, rig.look);

    doc::UndoStack undo;
    doc::Group g;
    g.id = d.next_effect_id++;
    std::vector<doc::EffectInstance> members;
    members.push_back(doc::make_effect(d, doc::EffectType::Grain));
    members.push_back(doc::make_effect(d, doc::EffectType::Posterize));
    const uint64_t m0 = members[0].id, m1 = members[1].id;
    const uint64_t gid = g.id;
    g.inputs = {d.next_effect_id++};
    g.face_out = m1;
    for (auto& member : members) member.group_id = gid;
    undo.execute(d, doc::insert_group_command(rig.look, g, std::move(members),
        {{g.inputs[0], m0, 0}, {m0, m1, 0}}));
    const doc::Group* placed = doc::find_group(look, gid);
    CHECK(placed && !placed->inputs.empty());
    const uint64_t slot0 = placed->inputs.front();
    undo.execute(d, doc::reconnect_command(rig.look, {media_layer, 0, 0},
                                           {m1, 0, 0}));
    undo.execute(d, doc::connect_command(rig.look, {media_layer, slot0, 0}));

    const auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].asset, rig.asset);
    CHECK_EQ(voice[0].layer, media_layer);
    uint64_t bottom_out = 0;
    for (const doc::NodeLink& l : look.links)
        if (l.to == 0 && l.to_port == 0) {
            bottom_out = l.from;
            break;
        }
    CHECK_EQ(bottom_out, m1);

    while (undo.can_undo()) undo.undo(d);
    const auto voice2 = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice2.size(), size_t{1});
    CHECK_EQ(voice2[0].asset, rig.asset);
}

TEST(flatten_audio_chains_ops_in_wire_order) {
    // Video effects in the chain pass audio through and make no node.
    // A bypassed hop drops out and the chain keeps flowing through it.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    const uint64_t src_id = look.sources[0].id;
    look.effects.push_back(
        doc::make_effect(d, doc::EffectType::AudioDelay));
    look.effects.push_back(
        doc::make_effect(d, doc::EffectType::Posterize));
    look.effects.push_back(
        doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t delay_id = look.effects[0].id;
    const uint64_t poster_id = look.effects[1].id;
    const uint64_t gain_id = look.effects[2].id;
    look.effects[2].params[0] = 0.5f;
    look.links = {{src_id, delay_id, 0},
                  {delay_id, poster_id, 0},
                  {poster_id, gain_id, 0},
                  {gain_id, 0, 0}};
    lay_audio_block(d, rig.look);

    doc::AudioProgram prog = doc::flatten_audio_program(d, d.root_sequence);
    CHECK(prog.root >= 0);
    if (prog.root < 0) return;
    const doc::AudioNode& hop = prog.nodes[prog.root];
    CHECK_EQ(hop.inputs.size(), size_t{1});
    const doc::AudioNode& gain = prog.nodes[hop.inputs[0]];
    CHECK(gain.has_op);
    CHECK(gain.op.type == doc::EffectType::AudioGain);
    CHECK_EQ(gain.op.params[0], 0.5f);
    CHECK_EQ(gain.inputs.size(), size_t{1});
    const doc::AudioNode& delay = prog.nodes[gain.inputs[0]];
    CHECK(delay.has_op);
    CHECK(delay.op.type == doc::EffectType::AudioDelay);
    CHECK_EQ(delay.inputs.size(), size_t{1});
    CHECK_EQ(prog.nodes[delay.inputs[0]].asset, rig.asset);

    look.effects[0].bypass = true;
    prog = doc::flatten_audio_program(d, d.root_sequence);
    CHECK(prog.root >= 0);
    if (prog.root < 0) return;
    const doc::AudioNode& hop2 = prog.nodes[prog.root];
    const doc::AudioNode& gain2 = prog.nodes[hop2.inputs[0]];
    CHECK(gain2.has_op);
    CHECK(gain2.op.type == doc::EffectType::AudioGain);
    CHECK_EQ(gain2.inputs.size(), size_t{1});
    CHECK_EQ(prog.nodes[gain2.inputs[0]].asset, rig.asset);
}

TEST(flatten_audio_split_output_reads_its_own_port) {
    // With split on, port 1 is the voice and unwired means silent.
    Rig rig(60);
    Document& d = rig.doc;
    lay_audio_block(d, rig.look);

    CHECK_EQ(doc::flatten_audio_sources(d, d.root_sequence).size(),
             size_t{1});
    d.looks[0].audio_split = true;
    CHECK_EQ(doc::flatten_audio_sources(d, d.root_sequence).size(),
             size_t{0});
    d.looks[0].links.push_back({d.looks[0].sources[0].id, 0, 1});
    CHECK_EQ(doc::flatten_audio_sources(d, d.root_sequence).size(),
             size_t{1});
}

TEST(flatten_audio_nested_program_composes_through_the_hop) {
    // The outer DSP sits above the nested hop and the inner DSP below it.
    Rig rig(60);
    Document& d = rig.doc;
    d.looks[0].effects.push_back(
        doc::make_effect(d, doc::EffectType::AudioDelay));
    d.looks[0].links.clear();
    connect_test_chain(d.looks[0], d.looks[0].sources[0].id, {d.looks[0].effects[0].id});

    doc::Look outer;
    outer.id = d.next_effect_id++;
    outer.name = "outer";
    doc::Source ref;
    ref.id = d.next_effect_id++;
    ref.source = doc::SourceKind::LookRef;
    ref.target = rig.look;
    outer.effects.push_back(doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t gain_id = outer.effects[0].id;
    const uint64_t ref_id = ref.id;
    outer.sources.push_back(std::move(ref));
    outer.links = {{ref_id, gain_id, 0}, {gain_id, 0, 0}};
    d.looks.push_back(std::move(outer));
    lay_audio_block(d, d.looks.back().id);

    const doc::AudioProgram prog =
        doc::flatten_audio_program(d, d.root_sequence);
    CHECK(prog.root >= 0);
    if (prog.root < 0) return;
    const doc::AudioNode& place = prog.nodes[prog.root];
    CHECK(place.windowed);
    const doc::AudioNode& gain = prog.nodes[place.inputs[0]];
    CHECK(gain.has_op);
    CHECK(gain.op.type == doc::EffectType::AudioGain);
    const doc::AudioNode& nest = prog.nodes[gain.inputs[0]];
    CHECK(nest.windowed);
    CHECK(!nest.has_op);
    const doc::AudioNode& delay = prog.nodes[nest.inputs[0]];
    CHECK(delay.has_op);
    CHECK(delay.op.type == doc::EffectType::AudioDelay);
    CHECK_EQ(prog.nodes[delay.inputs[0]].asset, rig.asset);

    const auto voice = doc::flatten_audio_sources(d, d.root_sequence);
    CHECK_EQ(voice.size(), size_t{1});
    CHECK_EQ(voice[0].asset, rig.asset);
}

TEST(resolve_audio_chain_composes_to_closed_form) {
    // The chain feeding a node counts the wired hop itself.
    // A sequence ref has no single voice and reads silent.
    Rig rig(60);
    Document& d = rig.doc;
    doc::Look& look = d.looks[0];
    look.sources[0].slip = 3;
    look.effects.push_back(
        doc::make_effect(d, doc::EffectType::AudioDelay));
    const uint64_t delay_id = look.effects[0].id;
    look.links = {{look.sources[0].id, delay_id, 0}, {delay_id, 0, 0}};
    const doc::AudioChain from_delay =
        doc::resolve_audio_chain(d, look, delay_id);
    CHECK_EQ(from_delay.asset, rig.asset);
    CHECK_EQ(from_delay.slip, uint32_t{3});
    CHECK_EQ(from_delay.op_count, uint32_t{1});
    CHECK(from_delay.ops[0].type == doc::EffectType::AudioDelay);
    const doc::AudioChain from_media =
        doc::resolve_audio_chain(d, look, look.sources[0].id);
    CHECK_EQ(from_media.asset, rig.asset);
    CHECK_EQ(from_media.op_count, uint32_t{0});

    doc::Look outer;
    outer.id = d.next_effect_id++;
    doc::Source ref;
    ref.id = d.next_effect_id++;
    ref.source = doc::SourceKind::LookRef;
    ref.target = rig.look;
    outer.effects.push_back(doc::make_effect(d, doc::EffectType::AudioGain));
    const uint64_t gain_id = outer.effects[0].id;
    const uint64_t ref_id = ref.id;
    outer.sources.push_back(std::move(ref));
    outer.links = {{ref_id, gain_id, 0}, {gain_id, 0, 0}};
    d.looks.push_back(std::move(outer));
    const doc::AudioChain nested =
        doc::resolve_audio_chain(d, d.looks.back(), gain_id);
    CHECK_EQ(nested.asset, rig.asset);
    CHECK_EQ(nested.slip, uint32_t{3});
    CHECK_EQ(nested.op_count, uint32_t{2});
    CHECK(nested.ops[0].type == doc::EffectType::AudioDelay);
    CHECK(nested.ops[1].type == doc::EffectType::AudioGain);

    doc::Source sref;
    sref.id = d.next_effect_id++;
    sref.source = doc::SourceKind::SequenceRef;
    sref.target = d.root_sequence;
    d.looks.back().sources.push_back(sref);
    CHECK_EQ(doc::resolve_audio_chain(d, d.looks.back(), sref.id).asset,
             uint64_t{0});
}

TEST(flatten_skips_hidden_and_dangling_sources) {
    Rig rig(50);
    rig.doc.looks[0].sources[0].visible = false;
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
    rig.doc.looks[0].sources[0].visible = true;
    rig.placement().target = 999999;   // dangling block: dormant
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
    rig.placement().target = rig.look;
    // A dangling asset id is dormant, like an unbound node.
    rig.doc.looks[0].sources[0].asset = 888888;
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{0});
}

TEST(canvas_size_derives_from_the_first_asset) {
    Document d = doc_with_look();
    uint32_t w = 0, h = 0;
    doc::canvas_size(d, &w, &h);
    CHECK_EQ(w, uint32_t{1920});
    CHECK_EQ(h, uint32_t{1080});
    doc::Asset a;
    a.id = d.next_effect_id++;
    a.width = 641;
    a.height = 480;
    d.assets.push_back(a);
    doc::canvas_size(d, &w, &h);
    CHECK_EQ(w, uint32_t{641});
    CHECK_EQ(h, uint32_t{480});
    d.canvas_w = 1280;
    d.canvas_h = 720;
    doc::canvas_size(d, &w, &h);
    CHECK_EQ(w, uint32_t{1280});
    CHECK_EQ(h, uint32_t{720});
}

TEST(flatten_conforms_mismatched_media_rate) {
    // A 30 fps media in a 60 fps project holds each frame twice.
    // The slip stays exact in media frames, so derived lengths double.
    Rig rig(100);
    rig.doc.fps = 60.0;
    rig.doc.assets[0].fps = 30.0;
    rig.doc.looks[0].sources[0].slip = 10;

    // 90 media frames past the slip = 180 clock frames.
    CHECK_EQ(doc::layer_source_length(rig.doc, rig.doc.looks[0].sources[0],
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

    // The mix maps by seconds, so the audio walk must not conform twice.
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
    // A cut conformed block keeps the same media frames and stream key.
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
    CHECK_EQ(a.key, b.key);
    for (uint32_t f = 91; f < 97; ++f)
        CHECK_EQ(doc::media_asset_frame(b, static_cast<double>(f)),
                 std::floor(static_cast<double>(f) * 0.5));
}

TEST(pinned_fps_look_conforms_through_the_hop) {
    // A pinned 30 fps look in a 60 fps project ticks its own clock.
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

    // The nested instance ticks the pinned clock: root 100 is local 50.
    const gfx::RenderGraph g =
        gfx::compile_graph(rig.doc, rig.doc.root_sequence, 100);
    bool found = false;
    for (const gfx::LookInstance& li : g.instances)
        if (li.look == rig.look) {
            found = true;
            CHECK_EQ(li.local_frame, uint32_t{50});
        }
    CHECK(found);

    // A cut on a child-frame boundary keeps the same media frames.
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
    // A still counts frames on the project clock, so its rate stays 1.
    Rig rig(150);
    rig.doc.fps = 60.0;
    rig.doc.assets[0].fps = 30.0;
    rig.doc.assets[0].still = true;
    CHECK_EQ(doc::media_conform_rate(rig.doc, rig.doc.assets[0], 60.0),
             1.0);
    CHECK_EQ(doc::layer_source_length(rig.doc, rig.doc.looks[0].sources[0],
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
    // A hidden lane's blocks leave the video flatten entirely.
    Rig rig(100);
    CHECK_EQ(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).size(),
        size_t{1});
    rig.doc.root().tracks[0].hidden = true;
    CHECK(
        doc::flatten_media_sources(rig.doc, rig.doc.root_sequence).empty());
}
