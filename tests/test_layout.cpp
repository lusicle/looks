#include "test_framework.h"
#include "ui/layout.h"
#include "ui/widgets.h"

using namespace looks;
using namespace looks::ui;

namespace {

struct Fixture {
    Canvas2D canvas;
    UiInput input;
    Context ctx;
    Font font = Font::create_debug();
    LayoutFrame frame{canvas, input, ctx, font, default_theme(), 1.0f / 60.0f};

    Fixture() { canvas.begin_frame(1.0f, {800, 600}); }
};

LayoutNode* fixed_box(LayoutArena& arena, float w, float h) {
    return SizedBox(arena, SizeSpec::fixed(w), SizeSpec::fixed(h), nullptr);
}

}  // namespace

TEST(layout_vstack_fixed_and_fill) {
    Fixture f;
    LayoutArena arena;
    LayoutNode* a = fixed_box(arena, 50, 30);
    LayoutNode* b = SizedBox(arena, SizeSpec::fill(), SizeSpec::fill(), nullptr);
    LayoutNode* c = SizedBox(arena, SizeSpec::fill(), SizeSpec::fill(3.0f), nullptr);
    LayoutNode* root = VStack(arena, {}, {a, b, c});
    layout(*root, {0, 0, 200, 130}, f.frame);

    CHECK_EQ(a->rect.h, 30.0f);
    CHECK_EQ(b->rect.h, 25.0f);    // (130-30) split 1:3
    CHECK_EQ(c->rect.h, 75.0f);
    CHECK_EQ(b->rect.y, 30.0f);
    CHECK_EQ(c->rect.y, 55.0f);
    CHECK_EQ(b->rect.w, 200.0f);
    CHECK_EQ(a->rect.w, 200.0f);   // stretch overrides fixed cross size
}

TEST(layout_hstack_gap_and_padding) {
    Fixture f;
    LayoutArena arena;
    LayoutNode* a = fixed_box(arena, 40, 20);
    LayoutNode* b = fixed_box(arena, 40, 20);
    StackOpts opts;
    opts.gap = 10;
    opts.padding = Edges::all(5);
    opts.cross_align = AlignMode::Start;
    LayoutNode* root = HStack(arena, opts, {a, b});
    layout(*root, {0, 0, 300, 100}, f.frame);

    CHECK_EQ(a->rect.x, 5.0f);
    CHECK_EQ(a->rect.y, 5.0f);
    CHECK_EQ(b->rect.x, 55.0f);    // 5 + 40 + 10
    CHECK_EQ(a->rect.w, 40.0f);
    CHECK_EQ(a->rect.h, 20.0f);
}

TEST(layout_justify_center_and_spacer) {
    Fixture f;
    LayoutArena arena;
    {
        LayoutNode* a = fixed_box(arena, 50, 10);
        StackOpts opts;
        opts.justify = Justify::Center;
        LayoutNode* root = HStack(arena, opts, {a});
        layout(*root, {0, 0, 200, 20}, f.frame);
        CHECK_EQ(a->rect.x, 75.0f);
    }
    {
        LayoutNode* a = fixed_box(arena, 50, 10);
        LayoutNode* b = fixed_box(arena, 50, 10);
        LayoutNode* root = HStack(arena, {}, {a, Spacer(arena), b});
        layout(*root, {0, 0, 300, 20}, f.frame);
        CHECK_EQ(a->rect.x, 0.0f);
        CHECK_EQ(b->rect.x, 250.0f);   // pushed to the far edge
    }
}

TEST(layout_auto_stack_hugs_content) {
    Fixture f;
    LayoutArena arena;
    StackOpts opts;
    opts.gap = 4;
    opts.cross_align = AlignMode::Start;
    LayoutNode* root = VStack(arena, opts,
                              {fixed_box(arena, 60, 20), fixed_box(arena, 80, 30)});
    const Vec2 size = measure(*root, Constraints::loose({500, 500}), f.frame);
    CHECK_EQ(size.y, 54.0f);   // 20 + 4 + 30
    CHECK_EQ(size.x, 80.0f);   // widest child
}

TEST(layout_percent_in_stack_and_padding_stretch) {
    Fixture f;
    LayoutArena arena;
    LayoutNode* inner = SizedBox(arena, SizeSpec::percent(0.5f),
                                 SizeSpec::fixed(40), nullptr);
    StackOpts opts;
    opts.padding = Edges::all(10);
    opts.cross_align = AlignMode::Start;
    LayoutNode* root = VStack(arena, opts, {inner});
    layout(*root, {0, 0, 220, 100}, f.frame);
    CHECK_EQ(inner->rect.x, 10.0f);
    CHECK_EQ(inner->rect.y, 10.0f);
    CHECK_EQ(inner->rect.w, 100.0f);   // 50% of (220 - 20)
    CHECK_EQ(inner->rect.h, 40.0f);

    // Padding stretches the child to the full inset rect.
    LayoutNode* boxed = SizedBox(arena, SizeSpec::fixed(30),
                                 SizeSpec::fixed(30), nullptr);
    LayoutNode* padded = Padding_(arena, Edges::all(10), boxed);
    layout(*padded, {0, 0, 220, 100}, f.frame);
    CHECK_EQ(boxed->rect.w, 200.0f);
    CHECK_EQ(boxed->rect.h, 80.0f);
}

TEST(layout_scroll_clamps_offset) {
    Fixture f;
    LayoutArena arena;
    ScrollState scroll;
    scroll.offset = 10000.0f;
    LayoutNode* tall = SizedBox(arena, SizeSpec::fill(), SizeSpec::fixed(1000),
                                nullptr);
    LayoutNode* root = ScrollAreaV(arena, &scroll, tall);
    layout(*root, {0, 0, 200, 300}, f.frame);
    CHECK_EQ(scroll.offset, 700.0f);       // content 1000 - viewport 300
    CHECK_EQ(tall->rect.y, -700.0f);
    CHECK_EQ(scroll.content, 1000.0f);
    CHECK_EQ(scroll.viewport, 300.0f);
    CHECK((root->clip == Rect{0, 0, 200, 300}));
    CHECK_EQ(tall->clip == root->clip, true);
}

TEST(layout_scrollbar_yields_to_a_popup) {
    Fixture f;
    LayoutArena arena;
    ScrollState scroll;
    bool cover = false;

    auto build_and_run = [&](Vec2 mouse, uint8_t pressed, uint8_t released,
                             uint8_t down) {
        arena.reset();
        f.canvas.begin_frame(1.0f, {800, 600});
        f.input.mouse = mouse;
        f.input.buttons_pressed = pressed;
        f.input.buttons_released = released;
        f.input.buttons_down = down;
        LayoutNode* tall = SizedBox(arena, SizeSpec::fill(),
                                    SizeSpec::fixed(1000), nullptr);
        LayoutNode* scroller = ScrollAreaV(arena, &scroll, tall);
        LayoutNode* popup = SizedBox(arena, SizeSpec::fill(), SizeSpec::fill(),
                                     nullptr);
        popup->user = &cover;
        popup->hit_fn = [](LayoutNode& node, LayoutFrame& fr) {
            if (!*static_cast<bool*>(node.user)) return;
            fr.ctx.add_hit(node.rect, fr.ctx.acquire_widget_id(node.user),
                           HitLayer::Popup);
        };
        LayoutNode* root = ZStack(arena, {scroller, popup});
        run_frame(root, {0, 0, 200, 300}, f.frame);
    };

    build_and_run({195, 20}, kMouseLeft, 0, kMouseLeft);
    build_and_run({195, 60}, 0, 0, kMouseLeft);
    CHECK(scroll.offset > 0.0f);
    build_and_run({195, 60}, 0, kMouseLeft, 0);
    build_and_run({195, 60}, 0, 0, 0);

    scroll.offset = 0.0f;
    cover = true;
    build_and_run({195, 20}, kMouseLeft, 0, kMouseLeft);
    build_and_run({195, 60}, 0, 0, kMouseLeft);
    CHECK_EQ(scroll.offset, 0.0f);
}

TEST(context_widget_id_stability_and_gc) {
    Context ctx;
    int state_a = 0, state_b = 0;

    ctx.begin_frame({});
    const WidgetId a1 = ctx.acquire_widget_id(&state_a);
    const WidgetId b1 = ctx.acquire_widget_id(&state_b);
    CHECK(!a1.is_null());
    CHECK(!(a1 == b1));
    ctx.gc_widget_slots();

    ctx.begin_frame({});
    const WidgetId a2 = ctx.acquire_widget_id(&state_a);
    CHECK(a1 == a2);               // same pointer, same frame-to-frame id
    ctx.gc_widget_slots();         // state_b not acquired -> recycled

    ctx.begin_frame({});
    const WidgetId b2 = ctx.acquire_widget_id(&state_b);
    CHECK(!(b1 == b2));            // generation bumped after recycle
    ctx.gc_widget_slots();
}

TEST(context_hit_winner_order_and_drag) {
    Context ctx;
    int state_a = 0, state_b = 0;

    ctx.begin_frame({{75, 75}, kMouseLeft, 0, 0, 0.0f});
    const WidgetId a = ctx.acquire_widget_id(&state_a);
    const WidgetId b = ctx.acquire_widget_id(&state_b);
    ctx.add_hit({0, 0, 100, 100}, a);
    ctx.add_hit({50, 50, 100, 100}, b);   // registered later -> on top
    ctx.finalize_hits();
    CHECK(ctx.hit_winner() == b);
    CHECK(ctx.widget_owns_mouse(b));
    CHECK(!ctx.widget_owns_mouse(a));

    ctx.begin_drag(a);
    CHECK(ctx.drag_active());
    CHECK(ctx.widget_owns_mouse(a));
    CHECK(!ctx.widget_owns_mouse(b));
    ctx.gc_widget_slots();

    ctx.begin_frame({{500, 500}, kMouseLeft, 0, 0, 0.0f});
    ctx.acquire_widget_id(&state_a);
    ctx.acquire_widget_id(&state_b);
    ctx.finalize_hits();
    CHECK(!ctx.any_hit());
    CHECK(ctx.widget_owns_mouse(a));
    ctx.gc_widget_slots();
}

TEST(context_modal_layer_is_exclusive) {
    Context ctx;
    int tree = 0, modal = 0;

    ctx.begin_frame({{10, 10}, 0, 0, 0, 0.0f});
    const WidgetId t = ctx.acquire_widget_id(&tree);
    const WidgetId m = ctx.acquire_widget_id(&modal);
    ctx.add_hit({0, 0, 100, 100}, t);
    ctx.push_overlay({200, 200, 100, 100}, m, true);
    ctx.finalize_hits();
    CHECK(ctx.modal_open());
    CHECK(!ctx.any_hit());
    CHECK(!ctx.widget_owns_mouse(t));
    ctx.gc_widget_slots();

    ctx.begin_frame({{250, 250}, 0, 0, 0, 0.0f});
    ctx.acquire_widget_id(&tree);
    ctx.acquire_widget_id(&modal);
    ctx.add_hit({0, 0, 400, 400}, t);
    ctx.push_overlay({200, 200, 100, 100}, m, true);
    ctx.finalize_hits();
    CHECK(ctx.hit_winner() == m);
    ctx.gc_widget_slots();
}

TEST(context_popup_layer_lets_a_press_outside_through) {
    Context ctx;
    int tree = 0, popup = 0;

    ctx.begin_frame({{10, 10}, 0, 0, 0, 0.0f});
    const WidgetId t = ctx.acquire_widget_id(&tree);
    const WidgetId p = ctx.acquire_widget_id(&popup);
    ctx.add_hit({0, 0, 100, 100}, t);
    ctx.push_overlay({200, 200, 100, 100}, p, false);
    ctx.finalize_hits();
    CHECK(!ctx.modal_open());
    CHECK(ctx.hit_winner() == t);
    ctx.gc_widget_slots();

    ctx.begin_frame({{250, 250}, 0, 0, 0, 0.0f});
    ctx.acquire_widget_id(&tree);
    ctx.acquire_widget_id(&popup);
    ctx.add_hit({0, 0, 400, 400}, t);
    ctx.push_overlay({200, 200, 100, 100}, p, false);
    ctx.finalize_hits();
    CHECK(ctx.hit_winner() == p);
    ctx.gc_widget_slots();
}

TEST(context_focus_survives_a_redraw_and_dies_with_its_widget) {
    Context ctx;
    int field = 0, other = 0;

    ctx.begin_frame({});
    const WidgetId f = ctx.acquire_widget_id(&field);
    ctx.acquire_widget_id(&other);
    ctx.set_focus(f);
    CHECK(ctx.has_focus(f));
    ctx.finalize_hits();
    ctx.gc_widget_slots();

    ctx.begin_frame({});
    CHECK(ctx.acquire_widget_id(&field) == f);
    ctx.acquire_widget_id(&other);
    ctx.finalize_hits();
    CHECK(ctx.has_focus(f));
    ctx.gc_widget_slots();

    ctx.begin_frame({});
    ctx.acquire_widget_id(&other);
    ctx.finalize_hits();
    ctx.gc_widget_slots();
    CHECK(!ctx.has_focus(f));
    CHECK(ctx.focus().is_null());
}

TEST(context_press_outside_the_focus_clears_it) {
    Context ctx;
    int field = 0, other = 0;

    auto frame = [&](Vec2 m, uint8_t pressed) {
        ctx.begin_frame({m, pressed, pressed, 0, 0.0f, 0.0f});
        const WidgetId f = ctx.acquire_widget_id(&field);
        const WidgetId o = ctx.acquire_widget_id(&other);
        ctx.add_hit({0, 0, 50, 50}, f);
        ctx.add_hit({100, 0, 50, 50}, o);
        ctx.finalize_hits();
        return f;
    };

    WidgetId f = frame({10, 10}, 0);
    ctx.set_focus(f);
    CHECK(ctx.has_focus(f));
    ctx.gc_widget_slots();

    frame({10, 10}, kMouseLeft);
    CHECK(ctx.has_focus(f));      // a press on the focus keeps it
    ctx.gc_widget_slots();

    frame({120, 10}, kMouseLeft);
    CHECK(!ctx.has_focus(f));
    ctx.gc_widget_slots();
}

TEST(context_wheel_goes_to_the_pointer_owner) {
    Context ctx;
    int tree = 0, popup = 0;

    ctx.begin_frame({{10, 10}, 0, 0, 0, 3.0f, 0.0f});
    const WidgetId t = ctx.acquire_widget_id(&tree);
    ctx.add_hit({0, 0, 100, 100}, t);
    ctx.finalize_hits();
    CHECK_EQ(ctx.take_wheel(t), 3.0f);
    CHECK_EQ(ctx.take_wheel(t), 0.0f);
    ctx.gc_widget_slots();

    ctx.begin_frame({{10, 10}, 0, 0, 0, 3.0f, 0.0f});
    ctx.acquire_widget_id(&tree);
    const WidgetId p = ctx.acquire_widget_id(&popup);
    ctx.add_hit({0, 0, 100, 100}, t);
    ctx.push_overlay({0, 0, 50, 50}, p, false);
    ctx.finalize_hits();
    CHECK(ctx.pointer_over_overlay());
    CHECK_EQ(ctx.take_wheel(t), 0.0f);
    CHECK_EQ(ctx.take_wheel_in_tree(), 0.0f);
    CHECK_EQ(ctx.take_wheel(p), 3.0f);
    ctx.gc_widget_slots();

    ctx.begin_frame({{10, 10}, 0, 0, 0, 3.0f, 0.0f});
    ctx.acquire_widget_id(&tree);
    ctx.acquire_widget_id(&popup);
    ctx.add_hit({0, 0, 100, 100}, t);
    ctx.push_overlay({200, 200, 50, 50}, p, true);
    ctx.finalize_hits();
    CHECK(ctx.pointer_over_overlay());
    CHECK_EQ(ctx.take_wheel_in_tree(), 0.0f);
    ctx.gc_widget_slots();
}

TEST(context_drag_ends_when_the_buttons_are_up) {
    Context ctx;
    int state_a = 0;

    ctx.begin_frame({{0, 0}, kMouseLeft, 0, 0, 0.0f});
    const WidgetId a = ctx.acquire_widget_id(&state_a);
    ctx.begin_drag(a);
    CHECK(ctx.drag_active());
    ctx.gc_widget_slots();

    ctx.begin_frame({{0, 0}, 0, 0, kMouseLeft, 0.0f});
    ctx.acquire_widget_id(&state_a);
    CHECK(ctx.drag_active());
    ctx.gc_widget_slots();

    ctx.begin_frame({});
    ctx.acquire_widget_id(&state_a);
    CHECK(!ctx.drag_active());
    ctx.gc_widget_slots();
}

TEST(context_drag_ends_when_the_release_is_lost) {
    Context ctx;
    int state_a = 0;

    ctx.begin_frame({{0, 0}, kMouseLeft, 0, 0, 0.0f});
    const WidgetId a = ctx.acquire_widget_id(&state_a);
    ctx.begin_drag(a);
    CHECK(ctx.drag_active());
    ctx.gc_widget_slots();

    ctx.begin_frame({});
    ctx.acquire_widget_id(&state_a);
    CHECK(!ctx.drag_active());
    CHECK(!ctx.widget_owns_mouse(a));
    ctx.gc_widget_slots();
}

TEST(context_gesture_press_click_and_double) {
    Context ctx;
    int state_a = 0;
    const Rect r{0, 0, 100, 100};

    auto step = [&](Vec2 m, uint8_t down, uint8_t pressed, uint8_t released) {
        ctx.begin_frame({m, down, pressed, released, 0.0f, 0.05f});
        const WidgetId id = ctx.acquire_widget_id(&state_a);
        ctx.add_hit(r, id);
        ctx.finalize_hits();
        const Gesture g = ctx.gesture(id, r);
        ctx.gc_widget_slots();
        return g;
    };
    auto click_at = [&](Vec2 m) {
        const Gesture down = step(m, kMouseLeft, kMouseLeft, 0);
        step(m, 0, 0, kMouseLeft);
        step(m, 0, 0, 0);
        return down;
    };

    Gesture g = step({10, 10}, kMouseLeft, kMouseLeft, 0);
    CHECK(g.pressed);
    CHECK(!g.double_clicked);
    CHECK(ctx.drag_active());

    g = step({10, 10}, 0, 0, kMouseLeft);
    CHECK(g.clicked);
    CHECK(g.drag_released);
    step({10, 10}, 0, 0, 0);

    g = click_at({10, 10});
    CHECK(g.double_clicked);

    click_at({10, 10});
    g = click_at({19, 10});
    CHECK(!g.double_clicked);
}

TEST(context_nested_targets_each_see_the_double) {
    Context ctx;
    int state_a = 0;
    const Rect r{0, 0, 100, 100};
    bool inner = false;

    auto step = [&](Vec2 m, uint8_t down, uint8_t pressed,
                    uint8_t released) {
        ctx.begin_frame({m, down, pressed, released, 0.0f, 0.05f});
        const WidgetId id = ctx.acquire_widget_id(&state_a);
        ctx.add_hit(r, id);
        ctx.finalize_hits();
        const Gesture g = ctx.gesture(id, r);
        if (g.pressed) inner = ctx.press_is_double(77);
        ctx.gc_widget_slots();
        return g;
    };
    auto click_at = [&](Vec2 m) {
        const Gesture down = step(m, kMouseLeft, kMouseLeft, 0);
        step(m, 0, 0, kMouseLeft);
        step(m, 0, 0, 0);
        return down;
    };

    click_at({10, 10});
    CHECK(!inner);
    const Gesture g = click_at({10, 10});
    CHECK(g.double_clicked);
    CHECK(inner);
}

TEST(context_gesture_drag_threshold) {
    Context ctx;
    int state_a = 0;
    const Rect r{0, 0, 100, 100};

    auto step = [&](Vec2 m, uint8_t down, uint8_t pressed, uint8_t released) {
        ctx.begin_frame({m, down, pressed, released, 0.0f, 0.05f});
        const WidgetId id = ctx.acquire_widget_id(&state_a);
        ctx.add_hit(r, id);
        ctx.finalize_hits();
        const Gesture g = ctx.gesture(id, r);
        ctx.gc_widget_slots();
        return g;
    };

    Gesture g = step({10, 10}, kMouseLeft, kMouseLeft, 0);
    CHECK(!g.drag_moved);
    g = step({14, 10}, kMouseLeft, 0, 0);
    CHECK(!g.drag_moved);
    g = step({20, 10}, kMouseLeft, 0, 0);
    CHECK(g.drag_started);
    CHECK(g.drag_moved);
    CHECK_EQ(g.drag_delta.x, 10.0f);
    g = step({30, 10}, kMouseLeft, 0, 0);
    CHECK(!g.drag_started);
    CHECK(g.drag_moved);
}

TEST(widget_button_click_flow) {
    Fixture f;
    LayoutArena arena;
    ButtonState bstate;
    bool clicked = false;

    auto build_and_run = [&](Vec2 mouse, uint8_t pressed, uint8_t released,
                             uint8_t down) {
        arena.reset();
        f.canvas.begin_frame(1.0f, {800, 600});
        f.input.mouse = mouse;
        f.input.buttons_pressed = pressed;
        f.input.buttons_released = released;
        f.input.buttons_down = down;
        clicked = false;
        StackOpts opts;
        opts.cross_align = AlignMode::Start;
        LayoutNode* root = VStack(
            arena, opts, {Button(arena, "go", &bstate, &clicked)});
        run_frame(root, {0, 0, 800, 600}, f.frame);
    };

    build_and_run({30, 10}, 0, 0, 0);
    CHECK(!clicked);
    CHECK(f.ctx.any_hit());        // hovering a widget claims the pointer
    build_and_run({30, 10}, kMouseLeft, 0, kMouseLeft);
    CHECK(bstate.pressed);
    CHECK(!clicked);
    build_and_run({30, 10}, 0, kMouseLeft, 0);
    CHECK(clicked);
    CHECK(!bstate.pressed);
    build_and_run({30, 10}, kMouseLeft, 0, kMouseLeft);
    build_and_run({700, 500}, 0, kMouseLeft, 0);
    CHECK(!clicked);
    CHECK(!bstate.pressed);
}
