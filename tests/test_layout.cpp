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

TEST(context_widget_id_stability_and_gc) {
    Context ctx;
    int state_a = 0, state_b = 0;

    ctx.begin_frame();
    const WidgetId a1 = ctx.acquire_widget_id(&state_a);
    const WidgetId b1 = ctx.acquire_widget_id(&state_b);
    CHECK(!a1.is_null());
    CHECK(!(a1 == b1));
    ctx.gc_widget_slots();

    ctx.begin_frame();
    const WidgetId a2 = ctx.acquire_widget_id(&state_a);
    CHECK(a1 == a2);               // same pointer, same frame-to-frame id
    ctx.gc_widget_slots();         // state_b not acquired -> recycled

    ctx.begin_frame();
    const WidgetId b2 = ctx.acquire_widget_id(&state_b);
    CHECK(!(b1 == b2));            // generation bumped after recycle
    ctx.gc_widget_slots();
}

TEST(context_hit_winner_order_and_capture) {
    Context ctx;
    int state_a = 0, state_b = 0;

    ctx.begin_frame();
    const WidgetId a = ctx.acquire_widget_id(&state_a);
    const WidgetId b = ctx.acquire_widget_id(&state_b);
    ctx.add_hit({0, 0, 100, 100}, a);
    ctx.add_hit({50, 50, 100, 100}, b);   // registered later -> on top
    ctx.finalize_hits({75, 75});
    CHECK(ctx.hit_winner() == b);
    CHECK(ctx.widget_owns_mouse(b));
    CHECK(!ctx.widget_owns_mouse(a));

    ctx.set_capture(a);
    CHECK(ctx.widget_owns_mouse(a));
    CHECK(!ctx.widget_owns_mouse(b));
    ctx.clear_capture();

    ctx.finalize_hits({500, 500});
    CHECK(!ctx.any_hit());
    ctx.gc_widget_slots();
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
        f.input.consumed = false;
        clicked = false;
        StackOpts opts;
        opts.cross_align = AlignMode::Start;
        LayoutNode* root = VStack(
            arena, opts, {Button(arena, "go", &bstate, &clicked)});
        run_frame(root, {0, 0, 800, 600}, f.frame);
    };

    build_and_run({30, 10}, 0, 0, 0);
    CHECK(!clicked);
    CHECK(f.input.consumed);       // hovering a widget claims the pointer
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
