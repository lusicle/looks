#include "gfx/graph.h"

#include "doc/effects.h"
#include "doc/layer_commands.h"
#include "doc_fixture.h"
#include "test_framework.h"
#include "util/hash.h"

using looks::doc::Document;
using looks::doc::EffectType;
using looks::doc::make_effect;
using looks::hash_combine;
using looks::gfx::compile_graph;
using looks::gfx::GraphNode;
using looks::gfx::RenderGraph;

namespace {

uint64_t add_inner_look(Document& d, EffectType fx_type) {
    looks::doc::Look inner;
    inner.id = d.next_effect_id++;
    inner.name = "inner";
    looks::doc::Source gen;
    gen.id = d.next_effect_id++;
    gen.source = looks::doc::SourceKind::Gradient;
    inner.effects.push_back(make_effect(d, fx_type));
    connect_test_chain(inner, gen.id, {inner.effects[0].id});
    inner.sources.push_back(std::move(gen));
    d.looks.push_back(std::move(inner));
    return d.looks.back().id;
}

uint64_t place_block(Document& d, uint64_t target, uint32_t t_in,
                     uint32_t t_out, float speed = 1.0f) {
    looks::doc::Placement place;
    place.id = d.next_effect_id++;
    place.target = target;
    place.t_in = t_in;
    place.t_out = t_out;
    place.speed = speed;
    d.root().tracks[0].placements.push_back(place);
    return place.id;
}

int count_effects(const RenderGraph& g) {
    int n = 0;
    for (const GraphNode& node : g.nodes)
        if (node.kind == GraphNode::Kind::Effect) ++n;
    return n;
}

}  // namespace

TEST(nesting_inlines_the_placed_look) {
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    place_block(d, inner, 0, 100);
    const RenderGraph g = compile_graph(d, d.root_sequence, 5);
    CHECK(g.valid);
    CHECK_EQ(count_effects(g), 1);
    // Two instances: the root sequence and the placed look.
    CHECK_EQ(g.instances.size(), size_t{2});
    CHECK_EQ(g.instances[1].look, inner);
}

TEST(nesting_instances_carry_local_clocks) {
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    place_block(d, inner, 10, 100);
    const RenderGraph g = compile_graph(d, d.root_sequence, 25);
    CHECK_EQ(g.instances.size(), size_t{2});
    // Block at 10: root 25 = local 15.
    CHECK_EQ(g.instances[1].local_frame, uint32_t{15});
}

TEST(nesting_speed_scales_the_local_clock) {
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    place_block(d, inner, 10, 100, 2.0f);
    const RenderGraph g = compile_graph(d, d.root_sequence, 25);
    CHECK_EQ(g.instances.size(), size_t{2});
    CHECK_EQ(g.instances[1].local_frame, uint32_t{30});
}

TEST(nesting_culls_instances_that_are_not_playing) {
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    place_block(d, inner, 10, 20);
    CHECK_EQ(compile_graph(d, d.root_sequence, 9).instances.size(),
             size_t{1});
    CHECK_EQ(compile_graph(d, d.root_sequence, 10).instances.size(),
             size_t{2});
    CHECK_EQ(compile_graph(d, d.root_sequence, 19).instances.size(),
             size_t{2});
    CHECK_EQ(compile_graph(d, d.root_sequence, 20).instances.size(),
             size_t{1});
}

TEST(nesting_two_blocks_of_one_look_share_razor_stable_keys) {
    // The path folds container and target ids, never placement ids.
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Feedback);
    place_block(d, inner, 0, 10);
    place_block(d, inner, 20, 30);
    const uint64_t lane = d.root().tracks[0].id;

    const RenderGraph a = compile_graph(d, d.root_sequence, 5);
    const RenderGraph b = compile_graph(d, d.root_sequence, 25);
    CHECK_EQ(a.instances.size(), size_t{2});
    CHECK_EQ(b.instances.size(), size_t{2});
    CHECK_EQ(a.instances[1].path, b.instances[1].path);
    CHECK_EQ(a.instances[1].path,
             hash_combine(hash_combine(d.root_sequence, lane), inner));

    looks::doc::SeqTrack lane2;
    lane2.id = d.next_effect_id++;
    looks::doc::Placement other;
    other.id = d.next_effect_id++;
    other.target = inner;
    other.t_in = 0;
    other.t_out = 10;
    lane2.placements.push_back(other);
    d.root().tracks.push_back(std::move(lane2));
    const RenderGraph c = compile_graph(d, d.root_sequence, 5);
    CHECK_EQ(c.instances.size(), size_t{3});
    CHECK(c.instances[1].path != c.instances[2].path);
}

TEST(nesting_lockstep_ref_inside_a_look) {
    // A look ref runs on the same clock, with no affine hop.
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    looks::doc::Source ref;
    ref.id = d.next_effect_id++;
    ref.source = looks::doc::SourceKind::LookRef;
    ref.target = inner;
    d.looks[0].sources.push_back(std::move(ref));
    const uint64_t ref_id = d.looks[0].sources.back().id;
    d.looks[0].links.push_back({ref_id, 0, 0});

    const RenderGraph g = compile_graph(d, d.looks[0].id, 33);
    CHECK(g.valid);
    CHECK_EQ(g.instances.size(), size_t{2});
    CHECK_EQ(g.instances[1].local_frame, uint32_t{33});
    CHECK_EQ(g.instances[1].path,
             hash_combine(hash_combine(d.looks[0].id, ref_id), inner));
}

TEST(nesting_sequence_inside_a_look_carries_its_lanes) {
    // A nested sequence resolves its own lanes on the look's clock.
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    looks::doc::Sequence cut;
    cut.id = d.next_effect_id++;
    looks::doc::SeqTrack lane;
    lane.id = d.next_effect_id++;
    looks::doc::Placement p;
    p.id = d.next_effect_id++;
    p.target = inner;
    p.t_in = 10;
    p.t_out = 20;
    lane.placements.push_back(p);
    cut.tracks.push_back(std::move(lane));
    const uint64_t cut_id = cut.id;
    d.sequences.push_back(std::move(cut));

    looks::doc::Look grade;
    grade.id = d.next_effect_id++;
    looks::doc::Source sref;
    sref.id = d.next_effect_id++;
    sref.source = looks::doc::SourceKind::SequenceRef;
    sref.target = cut_id;
    grade.effects.push_back(make_effect(d, EffectType::Grain));
    connect_test_chain(grade, sref.id, {grade.effects[0].id});
    grade.sources.push_back(std::move(sref));
    const uint64_t grade_id = grade.id;
    d.looks.push_back(std::move(grade));

    // At 15 the block plays: grade -> cut -> inner, and both effects emit.
    const RenderGraph g = compile_graph(d, grade_id, 15);
    CHECK(g.valid);
    CHECK_EQ(g.instances.size(), size_t{3});
    CHECK_EQ(count_effects(g), 2);
    // At 25 the block ended, so the inner instance never spawns.
    const RenderGraph late = compile_graph(d, grade_id, 25);
    CHECK_EQ(late.instances.size(), size_t{2});
    CHECK_EQ(count_effects(late), 0);
}

TEST(nesting_stops_at_the_depth_bound) {
    // Commands cannot build a self-cycle, but a hand-edited file can.
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    looks::doc::Source self_ref;
    self_ref.id = d.next_effect_id++;
    self_ref.source = looks::doc::SourceKind::LookRef;
    self_ref.target = inner;
    d.look(inner).sources.push_back(std::move(self_ref));
    place_block(d, inner, 0, 100);
    const RenderGraph g = compile_graph(d, d.root_sequence, 5);
    CHECK(g.valid);
    CHECK(g.instances.size() <=
          static_cast<size_t>(looks::doc::kMaxLookDepth) + 1);
}

TEST(nesting_dangling_reference_is_dormant) {
    Document d = doc_with_look();
    place_block(d, 999999, 0, 100);
    const RenderGraph g = compile_graph(d, d.root_sequence, 5);
    CHECK(g.valid);
    CHECK_EQ(g.instances.size(), size_t{1});
    // The composite is the black display node, not a fabricated frame.
    CHECK(g.nodes[static_cast<size_t>(g.output)].kind ==
          GraphNode::Kind::Generator);
}

TEST(nesting_reaches_spans_both_entity_kinds) {
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    place_block(d, inner, 0, 100);
    CHECK(looks::doc::nest_reaches(d, d.root_sequence, inner));
    CHECK(!looks::doc::nest_reaches(d, inner, d.root_sequence));
    CHECK(looks::doc::nest_reaches(d, inner, inner));
    looks::doc::Sequence cut;
    cut.id = d.next_effect_id++;
    looks::doc::SeqTrack lane;
    lane.id = d.next_effect_id++;
    looks::doc::Placement p;
    p.id = d.next_effect_id++;
    p.target = inner;
    lane.placements.push_back(p);
    cut.tracks.push_back(std::move(lane));
    const uint64_t cut_id = cut.id;
    d.sequences.push_back(std::move(cut));
    looks::doc::Source sref;
    sref.id = d.next_effect_id++;
    sref.source = looks::doc::SourceKind::SequenceRef;
    sref.target = cut_id;
    d.looks[0].sources.push_back(std::move(sref));
    CHECK(looks::doc::nest_reaches(d, d.looks[0].id, cut_id));
    CHECK(looks::doc::nest_reaches(d, d.looks[0].id, inner));
    CHECK(!looks::doc::nest_reaches(d, inner, d.looks[0].id));
}

TEST(nesting_two_levels_compose_their_maps) {
    // The lockstep ref passes the block's affine map straight through.
    Document d = doc_with_look();
    const uint64_t inner = add_inner_look(d, EffectType::Vignette);
    looks::doc::Look mid;
    mid.id = d.next_effect_id++;
    looks::doc::Source ref;
    ref.id = d.next_effect_id++;
    ref.source = looks::doc::SourceKind::LookRef;
    ref.target = inner;
    mid.sources.push_back(std::move(ref));
    const uint64_t mid_id = mid.id;
    d.looks.push_back(std::move(mid));
    place_block(d, mid_id, 10, 100, 2.0f);

    const RenderGraph g = compile_graph(d, d.root_sequence, 25);
    CHECK(g.valid);
    CHECK_EQ(g.instances.size(), size_t{3});
    CHECK_EQ(g.instances[1].local_frame, uint32_t{30});
    CHECK_EQ(g.instances[2].local_frame, uint32_t{30});
}
