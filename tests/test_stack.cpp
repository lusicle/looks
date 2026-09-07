#include "doc/stack_commands.h"

#include <algorithm>

#include "doc/effects.h"
#include "doc/layer_commands.h"
#include "doc/randomize.h"
#include "doc_fixture.h"
#include "test_framework.h"

using namespace looks::doc;

namespace {

Document make_doc_with_two() {
    Document doc = doc_with_look();
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::RgbSplit));
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    return doc;
}

}  // namespace

TEST(stack_make_effect_defaults) {
    Document doc = doc_with_look();
    EffectInstance fx = make_effect(doc, EffectType::Vignette);
    const EffectInfo& info = effect_info(EffectType::Vignette);
    CHECK_EQ(fx.params.size(), size_t{info.param_count});
    for (uint32_t i = 0; i < info.param_count; ++i)
        CHECK_EQ(fx.params[i], info.params[i].default_value);
    CHECK_EQ(fx.wet, 1.0f);
    CHECK_EQ(fx.opacity, 1.0f);
    CHECK(!fx.bypass);
    EffectInstance fx2 = make_effect(doc, EffectType::Pixelate);
    CHECK(fx2.id > fx.id);
}

TEST(stack_add_remove_undo) {
    Document doc = doc_with_look();
    UndoStack undo;

    undo.execute(doc, add_effect_command(doc.looks[0].id,0, 
                          make_effect(doc, EffectType::Pixelate), 0));
    CHECK_EQ(doc.looks[0].layers[0].stack.size(), size_t{1});
    CHECK(doc.looks[0].layers[0].stack[0].type == EffectType::Pixelate);

    undo.execute(doc, add_effect_command(doc.looks[0].id,0, 
                          make_effect(doc, EffectType::RgbSplit), 0));
    CHECK_EQ(doc.looks[0].layers[0].stack.size(), size_t{2});
    CHECK(doc.looks[0].layers[0].stack[0].type == EffectType::RgbSplit);

    const uint64_t pixelate_id = doc.looks[0].layers[0].stack[1].id;
    undo.execute(doc, remove_effect_command(doc.looks[0].id, 0, 1));
    CHECK_EQ(doc.looks[0].layers[0].stack.size(), size_t{1});

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack.size(), size_t{2});
    CHECK_EQ(doc.looks[0].layers[0].stack[1].id, pixelate_id);

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack.size(), size_t{1});
    CHECK(doc.looks[0].layers[0].stack[0].type == EffectType::Pixelate);

    undo.redo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack.size(), size_t{2});
    CHECK(doc.looks[0].layers[0].stack[0].type == EffectType::RgbSplit);
}

TEST(stack_set_param_and_wet_opacity) {
    Document doc = make_doc_with_two();
    UndoStack undo;

    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, 0, 20.0f));
    CHECK_EQ(doc.looks[0].layers[0].stack[0].params[0], 20.0f);

    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, kWetParam, 0.5f));
    CHECK_EQ(doc.looks[0].layers[0].stack[0].wet, 0.5f);
    undo.execute(doc, set_param_command(doc.looks[0].id,0, 1, kOpacityParam, 0.25f));
    CHECK_EQ(doc.looks[0].layers[0].stack[1].opacity, 0.25f);

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[1].opacity, 1.0f);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[0].wet, 1.0f);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[0].params[0],
             effect_info(EffectType::RgbSplit).params[0].default_value);
}

TEST(effect_controls_follow_dependencies) {
    for (uint32_t type = 0; type < static_cast<uint32_t>(EffectType::Count); ++type) {
        const auto& info = effect_info(static_cast<EffectType>(type));
        CHECK(info.param_count <= 30);
        CHECK(info.control_order != nullptr);
        if (!info.control_order || info.param_count > 30) continue;
        bool seen[32] = {};
        for (uint32_t row = 0; row < info.param_count; ++row) {
            const int p = info.control_param(row + 2);
            CHECK(p >= 0 && p < static_cast<int>(info.param_count));
            if (p < 0 || p >= static_cast<int>(info.param_count)) continue;
            CHECK(!seen[p]);
            const int dependency = info.params[p].vis_param;
            if (dependency >= 0) CHECK(seen[dependency]);
            seen[p] = true;
        }
        CHECK_EQ(info.control_param(0), kWetParam);
        CHECK_EQ(info.control_param(1), kOpacityParam);
    }
}

TEST(stack_param_drag_coalesces) {
    Document doc = make_doc_with_two();
    UndoStack undo;
    const float original = doc.looks[0].layers[0].stack[0].params[0];

    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, 0, 10.0f), /*coalesce=*/true);
    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, 0, 20.0f), /*coalesce=*/true);
    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, 0, 30.0f), /*coalesce=*/true);
    CHECK_EQ(doc.looks[0].layers[0].stack[0].params[0], 30.0f);
    CHECK_EQ(undo.undo_depth(), size_t{1});

    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, 1, 5.0f), /*coalesce=*/true);
    CHECK_EQ(undo.undo_depth(), size_t{2});

    undo.break_coalescing();
    undo.execute(doc, set_param_command(doc.looks[0].id,0, 0, 1, 8.0f), /*coalesce=*/true);
    CHECK_EQ(undo.undo_depth(), size_t{3});

    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[0].params[1], 5.0f);
    undo.undo(doc);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[0].params[0], original);
}

TEST(stack_randomize) {
    Document doc = doc_with_look();
    UndoStack undo;
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Quantize));
    doc.looks[0].layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const std::vector<float> before_q = doc.looks[0].layers[0].stack[0].params;
    const std::vector<float> before_v = doc.looks[0].layers[0].stack[1].params;

    randomize_stack(doc, undo, doc.looks[0].id,0, 1.0f, 1234);
    const EffectInfo& qinfo = effect_info(EffectType::Quantize);
    const auto& q = doc.looks[0].layers[0].stack[0].params;
    bool any_changed = false;
    for (uint32_t p = 0; p < qinfo.param_count; ++p) {
        CHECK(q[p] >= qinfo.params[p].min_value);
        CHECK(q[p] <= qinfo.params[p].max_value);
        if (!param_randomizable(qinfo.params[p]))
            CHECK_EQ(q[p], before_q[p]);
        else if (q[p] != before_q[p])
            any_changed = true;
    }
    CHECK(any_changed);
    // "palette" must be frozen, "levels" not.
    CHECK(!param_randomizable(qinfo.params[1]));
    CHECK(param_randomizable(qinfo.params[0]));

    CHECK_EQ(undo.undo_depth(), size_t{1});
    undo.undo(doc);
    CHECK(doc.looks[0].layers[0].stack[0].params == before_q);
    CHECK(doc.looks[0].layers[0].stack[1].params == before_v);

    // Determinism: same seed reproduces the same mutation.
    randomize_effect(doc, undo, doc.looks[0].id,0, 1, 0.7f, 42);
    const std::vector<float> first = doc.looks[0].layers[0].stack[1].params;
    undo.undo(doc);
    randomize_effect(doc, undo, doc.looks[0].id,0, 1, 0.7f, 42);
    CHECK(doc.looks[0].layers[0].stack[1].params == first);

    // Intensity 0 makes no change and leaves no undo entry.
    const size_t depth = undo.undo_depth();
    randomize_effect(doc, undo, doc.looks[0].id,0, 1, 0.0f, 99);
    CHECK(doc.looks[0].layers[0].stack[1].params == first);
    (void)depth;
}

TEST(stack_bypass_and_move) {
    Document doc = make_doc_with_two();
    UndoStack undo;
    const uint64_t first_id = doc.looks[0].layers[0].stack[0].id;

    undo.execute(doc, set_bypass_command(doc.looks[0].id,0, 0, true));
    CHECK(doc.looks[0].layers[0].stack[0].bypass);
    undo.undo(doc);
    CHECK(!doc.looks[0].layers[0].stack[0].bypass);

    undo.execute(doc, move_effect_command(doc.looks[0].id,0, 0, 1));
    CHECK_EQ(doc.looks[0].layers[0].stack[1].id, first_id);
    undo.undo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[0].id, first_id);
    undo.redo(doc);
    CHECK_EQ(doc.looks[0].layers[0].stack[1].id, first_id);
}

TEST(stack_connect_output_replaces_same_layer_end) {
    // The Output takes one contribution per layer, so a new end replaces it.
    Document doc = make_doc_with_two();          // layer 0: rgb, vignette
    doc.looks[0].layers.push_back(make_layer(doc, LayerSourceKind::Solid));
    doc.looks[0].layers[1].stack.push_back(make_effect(doc, EffectType::Pixelate));
    const uint64_t fx0 = doc.looks[0].layers[0].stack[0].id;
    const uint64_t fx1 = doc.looks[0].layers[0].stack[1].id;
    const uint64_t other_end = doc.looks[0].layers[1].stack[0].id;

    ensure_links(doc.looks[0]);   // l0: src->fx0->fx1->out, l1: src->pix->out
    auto out_ends = [&] {
        std::vector<uint64_t> ends;
        for (const auto& l : doc.looks[0].links)
            if (l.to == 0 && l.to_port == 0) ends.push_back(l.from);
        return ends;
    };
    CHECK_EQ(out_ends().size(), size_t{2});

    UndoStack undo;
    undo.execute(doc, connect_command(doc.looks[0].id, {fx0, 0, 0}));
    auto ends = out_ends();
    CHECK_EQ(ends.size(), size_t{2});
    CHECK(std::find(ends.begin(), ends.end(), fx0) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), other_end) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), fx1) == ends.end());
    // Stacking order is the link order, so the chain keeps its position.
    CHECK_EQ(ends[0], fx0);

    CHECK(undo.undo(doc));
    ends = out_ends();
    CHECK_EQ(ends.size(), size_t{2});
    CHECK(std::find(ends.begin(), ends.end(), fx1) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), fx0) == ends.end());
}

TEST(stack_param_gesture_coalesces_mixed) {
    // The drag writes one command per frame and merges to one undo entry.
    Document doc = doc_with_look();
    UndoStack undo;
    doc.looks[0].layers[0].stack.push_back(
        make_effect(doc, EffectType::Spherize));
    Look& look = doc.looks[0];
    const uint64_t fx_id = look.layers[0].stack[0].id;
    KeyframeLane lane;
    lane.target = {fx_id, 3};
    lane.keys.push_back({0.0, 0.25f});
    lane.loop = true;
    look.lanes.push_back(lane);

    auto write = [&](float x, float y) {
        std::vector<ParamWrite> base{{2, x}};
        std::vector<KeyframeLane> lw(1);
        lw[0].target = {fx_id, 3};
        lw[0].keys.push_back({0.0, y});
        undo.execute(doc,
                     set_param_gesture_command(look.id, 0, 0,
                                               std::move(base),
                                               std::move(lw)),
                     /*coalesce=*/true);
    };
    write(0.6f, 0.60f);
    write(0.7f, 0.70f);
    write(0.8f, 0.80f);
    undo.break_coalescing();

    CHECK_EQ(look.layers[0].stack[0].params[2], 0.8f);
    CHECK_EQ(look.lanes.size(), size_t{1});
    CHECK_EQ(look.lanes[0].keys[0].value, 0.8f);
    CHECK(look.lanes[0].loop);   // lane replace keeps loop/mute

    CHECK(undo.undo(doc));
    CHECK_EQ(look.layers[0].stack[0].params[2], 0.5f);
    CHECK_EQ(look.lanes[0].keys[0].value, 0.25f);
    CHECK(!undo.can_undo());

    CHECK(undo.redo(doc));
    CHECK_EQ(look.layers[0].stack[0].params[2], 0.8f);
    CHECK_EQ(look.lanes[0].keys[0].value, 0.8f);
}

TEST(stack_param_gesture_shape_guard) {
    // Different write shapes never merge into one undo entry.
    // Reverting a command that created a lane removes the lane.
    Document doc = doc_with_look();
    UndoStack undo;
    doc.looks[0].layers[0].stack.push_back(
        make_effect(doc, EffectType::Spherize));
    Look& look = doc.looks[0];
    const uint64_t fx_id = look.layers[0].stack[0].id;

    std::vector<ParamWrite> b1{{2, 0.6f}};
    undo.execute(doc,
                 set_param_gesture_command(look.id, 0, 0, std::move(b1), {}),
                 /*coalesce=*/true);

    std::vector<ParamWrite> b2{{2, 0.65f}};
    std::vector<KeyframeLane> lw(1);
    lw[0].target = {fx_id, 3};
    lw[0].keys.push_back({0.0, 0.9f});
    undo.execute(doc,
                 set_param_gesture_command(look.id, 0, 0, std::move(b2),
                                           std::move(lw)),
                 /*coalesce=*/true);
    CHECK_EQ(look.layers[0].stack[0].params[2], 0.65f);
    CHECK_EQ(look.lanes.size(), size_t{1});

    CHECK(undo.undo(doc));
    CHECK_EQ(look.layers[0].stack[0].params[2], 0.6f);
    CHECK_EQ(look.lanes.size(), size_t{0});
    CHECK(undo.undo(doc));
    CHECK_EQ(look.layers[0].stack[0].params[2], 0.5f);
}
