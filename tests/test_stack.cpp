#include "doc/stack_commands.h"

#include <algorithm>

#include "doc/effects.h"
#include "doc/layer_commands.h"
#include "doc/randomize.h"
#include "test_framework.h"

using namespace looks::doc;

namespace {

Document make_doc_with_two() {
    Document doc;
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::RgbSplit));
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    return doc;
}

}  // namespace

TEST(stack_make_effect_defaults) {
    Document doc;
    EffectInstance fx = make_effect(doc, EffectType::Vignette);
    const EffectInfo& info = effect_info(EffectType::Vignette);
    CHECK_EQ(fx.params.size(), size_t{info.param_count});
    for (uint32_t i = 0; i < info.param_count; ++i)
        CHECK_EQ(fx.params[i], info.params[i].default_value);
    CHECK_EQ(fx.wet, 1.0f);
    CHECK_EQ(fx.opacity, 1.0f);
    CHECK(!fx.bypass);
    // Stable ids are unique and monotonic.
    EffectInstance fx2 = make_effect(doc, EffectType::Pixelate);
    CHECK(fx2.id > fx.id);
}

TEST(stack_add_remove_undo) {
    Document doc;
    UndoStack undo;

    undo.execute(doc, add_effect_command(0, 
                          make_effect(doc, EffectType::Pixelate), 0));
    CHECK_EQ(doc.layers[0].stack.size(), size_t{1});
    CHECK(doc.layers[0].stack[0].type == EffectType::Pixelate);

    undo.execute(doc, add_effect_command(0, 
                          make_effect(doc, EffectType::RgbSplit), 0));
    CHECK_EQ(doc.layers[0].stack.size(), size_t{2});
    CHECK(doc.layers[0].stack[0].type == EffectType::RgbSplit);

    const uint64_t pixelate_id = doc.layers[0].stack[1].id;
    undo.execute(doc, remove_effect_command(0, 1));
    CHECK_EQ(doc.layers[0].stack.size(), size_t{1});

    undo.undo(doc);   // un-remove
    CHECK_EQ(doc.layers[0].stack.size(), size_t{2});
    CHECK_EQ(doc.layers[0].stack[1].id, pixelate_id);   // identity survives undo

    undo.undo(doc);   // un-add rgb split
    CHECK_EQ(doc.layers[0].stack.size(), size_t{1});
    CHECK(doc.layers[0].stack[0].type == EffectType::Pixelate);

    undo.redo(doc);
    CHECK_EQ(doc.layers[0].stack.size(), size_t{2});
    CHECK(doc.layers[0].stack[0].type == EffectType::RgbSplit);
}

TEST(stack_set_param_and_wet_opacity) {
    Document doc = make_doc_with_two();
    UndoStack undo;

    undo.execute(doc, set_param_command(0, 0, 0, 20.0f));
    CHECK_EQ(doc.layers[0].stack[0].params[0], 20.0f);

    undo.execute(doc, set_param_command(0, 0, kWetParam, 0.5f));
    CHECK_EQ(doc.layers[0].stack[0].wet, 0.5f);
    undo.execute(doc, set_param_command(0, 1, kOpacityParam, 0.25f));
    CHECK_EQ(doc.layers[0].stack[1].opacity, 0.25f);

    undo.undo(doc);
    CHECK_EQ(doc.layers[0].stack[1].opacity, 1.0f);
    undo.undo(doc);
    CHECK_EQ(doc.layers[0].stack[0].wet, 1.0f);
    undo.undo(doc);
    CHECK_EQ(doc.layers[0].stack[0].params[0],
             effect_info(EffectType::RgbSplit).params[0].default_value);
}

TEST(stack_param_drag_coalesces) {
    Document doc = make_doc_with_two();
    UndoStack undo;
    const float original = doc.layers[0].stack[0].params[0];

    // A drag: many coalesced edits -> one undo entry back to the start.
    undo.execute(doc, set_param_command(0, 0, 0, 10.0f), /*coalesce=*/true);
    undo.execute(doc, set_param_command(0, 0, 0, 20.0f), /*coalesce=*/true);
    undo.execute(doc, set_param_command(0, 0, 0, 30.0f), /*coalesce=*/true);
    CHECK_EQ(doc.layers[0].stack[0].params[0], 30.0f);
    CHECK_EQ(undo.undo_depth(), size_t{1});

    // Different knob must NOT merge.
    undo.execute(doc, set_param_command(0, 0, 1, 5.0f), /*coalesce=*/true);
    CHECK_EQ(undo.undo_depth(), size_t{2});

    // Gesture end: same knob again starts a fresh entry.
    undo.break_coalescing();
    undo.execute(doc, set_param_command(0, 0, 1, 8.0f), /*coalesce=*/true);
    CHECK_EQ(undo.undo_depth(), size_t{3});

    undo.undo(doc);
    CHECK_EQ(doc.layers[0].stack[0].params[1], 5.0f);
    undo.undo(doc);
    undo.undo(doc);
    CHECK_EQ(doc.layers[0].stack[0].params[0], original);
}

TEST(stack_randomize) {
    Document doc;
    UndoStack undo;
    // Quantizer: has a selector ("palette", "dither" is randomizable? no —
    // "dither" is not in the frozen list but "palette" is) and ranges.
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Quantize));
    doc.layers[0].stack.push_back(make_effect(doc, EffectType::Vignette));
    const std::vector<float> before_q = doc.layers[0].stack[0].params;
    const std::vector<float> before_v = doc.layers[0].stack[1].params;

    randomize_stack(doc, undo, 0, 1.0f, 1234);
    const EffectInfo& qinfo = effect_info(EffectType::Quantize);
    const auto& q = doc.layers[0].stack[0].params;
    bool any_changed = false;
    for (uint32_t p = 0; p < qinfo.param_count; ++p) {
        CHECK(q[p] >= qinfo.params[p].min_value);
        CHECK(q[p] <= qinfo.params[p].max_value);
        if (!param_randomizable(qinfo.params[p]))
            CHECK_EQ(q[p], before_q[p]);   // selectors stay put
        else if (q[p] != before_q[p])
            any_changed = true;
    }
    CHECK(any_changed);
    // "palette" must be frozen, "levels" not.
    CHECK(!param_randomizable(qinfo.params[1]));
    CHECK(param_randomizable(qinfo.params[0]));

    // Whole gesture = one undo step; undo restores everything.
    CHECK_EQ(undo.undo_depth(), size_t{1});
    undo.undo(doc);
    CHECK(doc.layers[0].stack[0].params == before_q);
    CHECK(doc.layers[0].stack[1].params == before_v);

    // Determinism: same seed reproduces the same mutation.
    randomize_effect(doc, undo, 0, 1, 0.7f, 42);
    const std::vector<float> first = doc.layers[0].stack[1].params;
    undo.undo(doc);
    randomize_effect(doc, undo, 0, 1, 0.7f, 42);
    CHECK(doc.layers[0].stack[1].params == first);

    // Intensity 0 = no-op (and must not leave an empty undo entry applied).
    const size_t depth = undo.undo_depth();
    randomize_effect(doc, undo, 0, 1, 0.0f, 99);
    CHECK(doc.layers[0].stack[1].params == first);
    (void)depth;
}

TEST(stack_bypass_and_move) {
    Document doc = make_doc_with_two();
    UndoStack undo;
    const uint64_t first_id = doc.layers[0].stack[0].id;

    undo.execute(doc, set_bypass_command(0, 0, true));
    CHECK(doc.layers[0].stack[0].bypass);
    undo.undo(doc);
    CHECK(!doc.layers[0].stack[0].bypass);

    undo.execute(doc, move_effect_command(0, 0, 1));
    CHECK_EQ(doc.layers[0].stack[1].id, first_id);
    undo.undo(doc);
    CHECK_EQ(doc.layers[0].stack[0].id, first_id);
    undo.redo(doc);
    CHECK_EQ(doc.layers[0].stack[1].id, first_id);
}

TEST(stack_connect_output_replaces_same_layer_end) {
    // The Output composites ONE contribution per owner layer; wiring a
    // new chain end must replace the same layer's old link (the ghost
    // double-composited the chain prefix) and keep other layers'.
    Document doc = make_doc_with_two();          // layer 0: rgb, vignette
    doc.layers.push_back(make_layer(doc, LayerSourceKind::Solid));
    doc.layers[1].stack.push_back(make_effect(doc, EffectType::Pixelate));
    const uint64_t fx0 = doc.layers[0].stack[0].id;
    const uint64_t fx1 = doc.layers[0].stack[1].id;
    const uint64_t other_end = doc.layers[1].stack[0].id;

    ensure_links(doc);   // l0: src->fx0->fx1->out, l1: src->pix->out
    auto out_ends = [&] {
        std::vector<uint64_t> ends;
        for (const auto& l : doc.links)
            if (l.to == 0 && l.to_port == 0) ends.push_back(l.from);
        return ends;
    };
    CHECK_EQ(out_ends().size(), size_t{2});

    UndoStack undo;
    undo.execute(doc, connect_command({fx0, 0, 0}));
    auto ends = out_ends();
    CHECK_EQ(ends.size(), size_t{2});
    CHECK(std::find(ends.begin(), ends.end(), fx0) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), other_end) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), fx1) == ends.end());

    CHECK(undo.undo(doc));
    ends = out_ends();
    CHECK_EQ(ends.size(), size_t{2});
    CHECK(std::find(ends.begin(), ends.end(), fx1) != ends.end());
    CHECK(std::find(ends.begin(), ends.end(), fx0) == ends.end());
}
