#include "ui/layout.h"

#include <algorithm>

namespace looks::ui {

namespace {

constexpr float kScrollPixelsPerNotch = 48.0f;
constexpr float kScrollbarWidth = 6.0f;
constexpr float kScrollbarPad = 2.0f;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Resolves one axis of a node's own SizeSpec against constraints, given the
// measured content extent. Fixed is authoritative; Fill stretches to the
// bound when one exists; Auto hugs content.
float resolve_axis(const SizeSpec& spec, float content, float min_c, float max_c) {
    const bool bounded = max_c < kUnboundedAxis * 0.5f;
    float v = content;
    switch (spec.mode) {
        case SizeMode::Fixed: v = spec.value; break;
        case SizeMode::Percent: v = bounded ? max_c * spec.value : content; break;
        case SizeMode::Fill: v = bounded ? max_c : content; break;
        case SizeMode::Auto: v = content; break;
    }
    return clampf(v, min_c, bounded ? max_c : v);
}

struct Axis {
    // main-axis accessors for VStack (y) / HStack (x)
    bool vertical;
    float main(Vec2 v) const { return vertical ? v.y : v.x; }
    float cross(Vec2 v) const { return vertical ? v.x : v.y; }
    Vec2 make(float main_v, float cross_v) const {
        return vertical ? Vec2{cross_v, main_v} : Vec2{main_v, cross_v};
    }
    const SizeSpec& main_spec(const LayoutNode& n) const {
        return vertical ? n.height : n.width;
    }
};

Vec2 measure_stack(LayoutNode& node, const Constraints& c,
                   const LayoutFrame& frame, bool vertical) {
    const Axis ax{vertical};
    const float pad_main = vertical ? node.padding.t + node.padding.b
                                    : node.padding.l + node.padding.r;
    const float pad_cross = vertical ? node.padding.l + node.padding.r
                                     : node.padding.t + node.padding.b;

    const float max_main = ax.main({c.max_w, c.max_h}) - pad_main;
    const float max_cross = ax.cross({c.max_w, c.max_h}) - pad_cross;
    const bool bounded_main = ax.main({c.max_w, c.max_h}) < kUnboundedAxis * 0.5f;

    Constraints child_loose;
    if (vertical) {
        child_loose = {0, max_cross, 0, kUnboundedAxis};
    } else {
        child_loose = {0, kUnboundedAxis, 0, max_cross};
    }

    // Pass A: non-Fill children measure to content; Fill children collect
    // weights.
    float fixed_main = 0.0f;
    float total_weight = 0.0f;
    float cross_extent = 0.0f;
    for (uint16_t i = 0; i < node.child_count; ++i) {
        LayoutNode& child = *node.children[i];
        const SizeSpec& spec = ax.main_spec(child);
        if (spec.mode == SizeMode::Fill && bounded_main) {
            total_weight += spec.value > 0.0f ? spec.value : 1.0f;
            continue;
        }
        const Vec2 size = measure(child, child_loose, frame);
        fixed_main += ax.main(size);
        cross_extent = std::max(cross_extent, ax.cross(size));
    }
    const float gaps = node.child_count > 1
        ? node.gap * static_cast<float>(node.child_count - 1) : 0.0f;

    // Pass B: distribute the remaining main extent across Fill children.
    if (total_weight > 0.0f) {
        const float remaining =
            std::max(0.0f, max_main - fixed_main - gaps);
        const float per_unit = remaining / total_weight;
        for (uint16_t i = 0; i < node.child_count; ++i) {
            LayoutNode& child = *node.children[i];
            const SizeSpec& spec = ax.main_spec(child);
            if (spec.mode != SizeMode::Fill) continue;
            const float weight = spec.value > 0.0f ? spec.value : 1.0f;
            const float share = per_unit * weight;
            Constraints cc = child_loose;
            if (vertical) { cc.min_h = cc.max_h = share; }
            else { cc.min_w = cc.max_w = share; }
            const Vec2 size = measure(child, cc, frame);
            fixed_main += ax.main(size);
            cross_extent = std::max(cross_extent, ax.cross(size));
        }
    }

    const float content_main = fixed_main + gaps + pad_main;
    const float content_cross = cross_extent + pad_cross;
    const Vec2 content = ax.make(content_main, content_cross);
    return {resolve_axis(node.width, content.x, c.min_w, c.max_w),
            resolve_axis(node.height, content.y, c.min_h, c.max_h)};
}

void arrange_stack(LayoutNode& node, const Rect& rect, const Rect& clip,
                   bool vertical) {
    const Axis ax{vertical};
    const Rect inner{rect.x + node.padding.l, rect.y + node.padding.t,
                     rect.w - node.padding.l - node.padding.r,
                     rect.h - node.padding.t - node.padding.b};
    const float slot_main = ax.main({inner.w, inner.h});
    const float slot_cross = ax.cross({inner.w, inner.h});

    // Fill children take exact shares of the ARRANGED slot (not their
    // measured desired) so a row can never overflow the rect it was given,
    // even when measure ran under different constraints.
    float used = 0.0f;
    float fill_weight = 0.0f;
    for (uint16_t i = 0; i < node.child_count; ++i) {
        const SizeSpec& spec = ax.main_spec(*node.children[i]);
        if (spec.mode == SizeMode::Fill)
            fill_weight += spec.value > 0.0f ? spec.value : 1.0f;
        else
            used += ax.main(node.children[i]->desired);
    }
    const float gaps = node.child_count > 1
        ? node.gap * static_cast<float>(node.child_count - 1) : 0.0f;
    const float fill_space = std::max(0.0f, slot_main - used - gaps);
    const float per_weight = fill_weight > 0.0f ? fill_space / fill_weight
                                                : 0.0f;
    float leftover = fill_weight > 0.0f ? 0.0f : fill_space;

    float cursor = 0.0f;
    float between = node.gap;
    switch (node.justify) {
        case Justify::Start: break;
        case Justify::Center: cursor = leftover * 0.5f; break;
        case Justify::End: cursor = leftover; break;
        case Justify::SpaceBetween:
            if (node.child_count > 1)
                between += leftover / static_cast<float>(node.child_count - 1);
            break;
        case Justify::SpaceAround: {
            const float unit = leftover / static_cast<float>(node.child_count);
            cursor = unit * 0.5f;
            between += unit;
            break;
        }
        case Justify::SpaceEvenly: {
            const float unit = leftover / static_cast<float>(node.child_count + 1);
            cursor = unit;
            between += unit;
            break;
        }
    }

    for (uint16_t i = 0; i < node.child_count; ++i) {
        LayoutNode& child = *node.children[i];
        const SizeSpec& spec = ax.main_spec(child);
        const float weight = spec.value > 0.0f ? spec.value : 1.0f;
        const float child_main = spec.mode == SizeMode::Fill
                                     ? per_weight * weight
                                     : ax.main(child.desired);
        float child_cross = ax.cross(child.desired);
        float cross_pos = 0.0f;
        switch (node.cross_align) {
            case AlignMode::Stretch: child_cross = slot_cross; break;
            case AlignMode::Start: break;
            case AlignMode::Center: cross_pos = (slot_cross - child_cross) * 0.5f; break;
            case AlignMode::End: cross_pos = slot_cross - child_cross; break;
        }
        const Vec2 pos = ax.make(cursor, cross_pos);
        const Vec2 size = ax.make(child_main, child_cross);
        arrange(child, {inner.x + pos.x, inner.y + pos.y, size.x, size.y}, clip);
        cursor += child_main + between;
    }
}

void route_wheel(LayoutNode& node, LayoutFrame& frame, LayoutNode** target) {
    if (node.kind == NodeKind::ScrollArea && node.scroll &&
        node.rect.contains(frame.input.mouse))
        *target = &node;   // deepest wins: children overwrite
    for (uint16_t i = 0; i < node.child_count; ++i)
        route_wheel(*node.children[i], frame, target);
}

void walk_hit(LayoutNode& node, LayoutFrame& frame) {
    if (node.hit_fn) node.hit_fn(node, frame);
    for (uint16_t i = 0; i < node.child_count; ++i)
        walk_hit(*node.children[i], frame);
}

void walk_draw(LayoutNode& node, LayoutFrame& frame, const Rect& active_clip) {
    const bool narrows = !node.clip.empty() && !(node.clip == active_clip);
    if (narrows) frame.canvas.push_clip(node.clip);
    if (node.draw_fn) node.draw_fn(node, frame);
    for (uint16_t i = 0; i < node.child_count; ++i)
        walk_draw(*node.children[i], frame, narrows ? node.clip : active_clip);
    if (narrows) frame.canvas.pop_clip();
}

void draw_scrollbar(LayoutNode& node, LayoutFrame& frame);

}  // namespace

Vec2 measure(LayoutNode& node, const Constraints& c, const LayoutFrame& frame) {
    Vec2 desired{};
    switch (node.kind) {
        case NodeKind::Leaf: {
            Vec2 content{};
            if (node.measure_fn) {
                Constraints inner = c;
                inner.max_w -= node.padding.l + node.padding.r;
                inner.max_h -= node.padding.t + node.padding.b;
                content = node.measure_fn(node, inner, frame);
                content.x += node.padding.l + node.padding.r;
                content.y += node.padding.t + node.padding.b;
            }
            desired = {resolve_axis(node.width, content.x, c.min_w, c.max_w),
                       resolve_axis(node.height, content.y, c.min_h, c.max_h)};
            break;
        }
        case NodeKind::VStack:
            desired = measure_stack(node, c, frame, true);
            break;
        case NodeKind::HStack:
            desired = measure_stack(node, c, frame, false);
            break;
        case NodeKind::ZStack:
        case NodeKind::Padding: {
            Constraints inner = c;
            inner.min_w = inner.min_h = 0;
            // A Fixed/Percent own size is a TIGHT bound for children
            // (SizedBox semantics). Without this, a fixed-width box in a
            // loose parent measures its children against the loose bound
            // and they overflow it at arrange.
            if (node.width.mode == SizeMode::Fixed)
                inner.max_w = std::min(node.width.value, c.max_w);
            else if (node.width.mode == SizeMode::Percent &&
                     c.max_w < kUnboundedAxis * 0.5f)
                inner.max_w = c.max_w * node.width.value;
            if (node.height.mode == SizeMode::Fixed)
                inner.max_h = std::min(node.height.value, c.max_h);
            else if (node.height.mode == SizeMode::Percent &&
                     c.max_h < kUnboundedAxis * 0.5f)
                inner.max_h = c.max_h * node.height.value;
            inner.max_w -= node.padding.l + node.padding.r;
            inner.max_h -= node.padding.t + node.padding.b;
            Vec2 content{};
            for (uint16_t i = 0; i < node.child_count; ++i) {
                const Vec2 s = measure(*node.children[i], inner, frame);
                content.x = std::max(content.x, s.x);
                content.y = std::max(content.y, s.y);
            }
            content.x += node.padding.l + node.padding.r;
            content.y += node.padding.t + node.padding.b;
            desired = {resolve_axis(node.width, content.x, c.min_w, c.max_w),
                       resolve_axis(node.height, content.y, c.min_h, c.max_h)};
            break;
        }
        case NodeKind::ScrollArea: {
            // Child measures against an unbounded main (vertical) axis.
            Constraints inner = c;
            inner.min_w = inner.min_h = 0;
            inner.max_h = kUnboundedAxis;
            inner.max_w -= kScrollbarWidth + kScrollbarPad * 2.0f;
            Vec2 content{};
            if (node.child_count > 0)
                content = measure(*node.children[0], inner, frame);
            if (node.scroll) node.scroll->content = content.y;
            desired = {resolve_axis(node.width, content.x, c.min_w, c.max_w),
                       resolve_axis(node.height, content.y, c.min_h, c.max_h)};
            break;
        }
    }
    node.desired = desired;
    return desired;
}

void arrange(LayoutNode& node, const Rect& rect, const Rect& parent_clip) {
    node.rect = rect;
    const bool narrows = node.kind == NodeKind::ScrollArea;
    node.clip = narrows
        ? (parent_clip.empty() ? rect : rect.intersect(parent_clip))
        : parent_clip;

    switch (node.kind) {
        case NodeKind::Leaf:
            break;
        case NodeKind::VStack:
            arrange_stack(node, rect, node.clip, true);
            break;
        case NodeKind::HStack:
            arrange_stack(node, rect, node.clip, false);
            break;
        case NodeKind::ZStack:
        case NodeKind::Padding: {
            const Rect inner{rect.x + node.padding.l, rect.y + node.padding.t,
                             rect.w - node.padding.l - node.padding.r,
                             rect.h - node.padding.t - node.padding.b};
            for (uint16_t i = 0; i < node.child_count; ++i)
                arrange(*node.children[i], inner, node.clip);
            break;
        }
        case NodeKind::ScrollArea: {
            if (node.child_count == 0) break;
            LayoutNode& child = *node.children[0];
            ScrollState* s = node.scroll;
            const float content = std::max(child.desired.y, rect.h);
            float offset = 0.0f;
            if (s) {
                s->viewport = rect.h;
                s->offset = clampf(s->offset, 0.0f, std::max(0.0f, content - rect.h));
                offset = s->offset;
            }
            const float inner_w = rect.w - kScrollbarWidth - kScrollbarPad * 2.0f;
            arrange(child, {rect.x, rect.y - offset, inner_w, content},
                    node.clip);
            break;
        }
    }
}

void layout(LayoutNode& root, const Rect& rect, const LayoutFrame& frame) {
    measure(root, Constraints::tight({rect.w, rect.h}), frame);
    arrange(root, rect, Rect{});
}

namespace {

void draw_scrollbar(LayoutNode& node, LayoutFrame& frame) {
    ScrollState* s = node.scroll;
    if (!s || s->content <= s->viewport + 0.5f) return;
    const Rect& r = node.rect;
    const Rect track{r.right() - kScrollbarWidth - kScrollbarPad,
                     r.y + kScrollbarPad, kScrollbarWidth,
                     r.h - kScrollbarPad * 2.0f};
    const float thumb_h =
        std::max(24.0f, track.h * (s->viewport / s->content));
    const float range = s->content - s->viewport;
    const float t = range > 0.0f ? s->offset / range : 0.0f;
    const Rect thumb{track.x, track.y + (track.h - thumb_h) * t, track.w,
                     thumb_h};

    // Thumb drag via capture keyed on the scroll state. The gutter has no
    // hit-test entries, so gate on the thumb rect + no live capture.
    const WidgetId id = frame.ctx.acquire_widget_id(s);
    if (frame.input.left_pressed() && !frame.ctx.has_capture() &&
        thumb.contains(frame.input.mouse)) {
        s->dragging_thumb = true;
        s->drag_grab = frame.input.mouse.y - thumb.y;
        frame.ctx.set_capture(id);
    }
    if (s->dragging_thumb) {
        if (!frame.input.left_down()) {
            s->dragging_thumb = false;
            frame.ctx.clear_capture();
        } else {
            const float new_top = frame.input.mouse.y - s->drag_grab - track.y;
            const float denom = track.h - thumb_h;
            s->offset = denom > 0.0f ? clampf(new_top / denom, 0.0f, 1.0f) * range
                                     : 0.0f;
        }
    }

    frame.canvas.draw_sdf_rect(track, kScrollbarWidth * 0.5f,
                               Color::hex(0x000000, 0.35f));
    frame.canvas.draw_sdf_rect(thumb, kScrollbarWidth * 0.5f,
                               Color::hex(0x5A5D63, s->dragging_thumb ? 1.0f : 0.8f));
}

}  // namespace

void run_frame(LayoutNode* root, const Rect& rect, LayoutFrame& frame) {
    if (!root) return;
    frame.ctx.begin_frame();
    layout(*root, rect, frame);

    // Hit pass, then winner resolution; a winner marks the pointer consumed.
    walk_hit(*root, frame);
    frame.ctx.finalize_hits(frame.input.mouse);
    if (frame.ctx.any_hit()) frame.input.consumed = true;

    // Wheel: innermost scroll area under the cursor takes the notches.
    if (frame.input.wheel_y != 0.0f) {
        LayoutNode* target = nullptr;
        route_wheel(*root, frame, &target);
        if (target && target->scroll) {
            target->scroll->offset -= frame.input.wheel_y * kScrollPixelsPerNotch;
            frame.input.wheel_y = 0.0f;
            frame.input.consumed = true;
        }
    }

    walk_draw(*root, frame, Rect{});
    frame.ctx.gc_widget_slots();
}

// ---- builders

LayoutNode* make_node(LayoutArena& arena, NodeKind kind) {
    LayoutNode* n = arena.alloc<LayoutNode>();
    n->kind = kind;
    n->cross_align = AlignMode::Stretch;
    return n;
}

LayoutNode* with_children(LayoutArena& arena, LayoutNode* node,
                          std::initializer_list<LayoutNode*> children) {
    size_t count = 0;
    for (LayoutNode* c : children)
        if (c) ++count;
    node->children = arena.alloc<LayoutNode*>(count);
    node->child_count = static_cast<uint16_t>(count);
    size_t i = 0;
    for (LayoutNode* c : children)
        if (c) node->children[i++] = c;
    return node;
}

static LayoutNode* make_stack(LayoutArena& arena, NodeKind kind,
                              const StackOpts& opts,
                              std::initializer_list<LayoutNode*> children) {
    LayoutNode* n = make_node(arena, kind);
    n->gap = opts.gap;
    n->padding = opts.padding;
    n->justify = opts.justify;
    n->cross_align = opts.cross_align;
    n->width = opts.width;
    n->height = opts.height;
    return with_children(arena, n, children);
}

static LayoutNode* make_stack_dyn(LayoutArena& arena, NodeKind kind,
                                  const StackOpts& opts,
                                  const std::vector<LayoutNode*>& children) {
    LayoutNode* n = make_node(arena, kind);
    n->gap = opts.gap;
    n->padding = opts.padding;
    n->justify = opts.justify;
    n->cross_align = opts.cross_align;
    n->width = opts.width;
    n->height = opts.height;
    size_t count = 0;
    for (LayoutNode* c : children)
        if (c) ++count;
    n->children = arena.alloc<LayoutNode*>(count);
    n->child_count = static_cast<uint16_t>(count);
    size_t i = 0;
    for (LayoutNode* c : children)
        if (c) n->children[i++] = c;
    return n;
}

LayoutNode* VStackDyn(LayoutArena& arena, const StackOpts& opts,
                      const std::vector<LayoutNode*>& children) {
    return make_stack_dyn(arena, NodeKind::VStack, opts, children);
}

LayoutNode* HStackDyn(LayoutArena& arena, const StackOpts& opts,
                      const std::vector<LayoutNode*>& children) {
    return make_stack_dyn(arena, NodeKind::HStack, opts, children);
}

LayoutNode* VStack(LayoutArena& arena, const StackOpts& opts,
                   std::initializer_list<LayoutNode*> children) {
    return make_stack(arena, NodeKind::VStack, opts, children);
}

LayoutNode* HStack(LayoutArena& arena, const StackOpts& opts,
                   std::initializer_list<LayoutNode*> children) {
    return make_stack(arena, NodeKind::HStack, opts, children);
}

LayoutNode* ZStack(LayoutArena& arena,
                   std::initializer_list<LayoutNode*> children) {
    return with_children(arena, make_node(arena, NodeKind::ZStack), children);
}

LayoutNode* Padding_(LayoutArena& arena, Edges edges, LayoutNode* child) {
    LayoutNode* n = make_node(arena, NodeKind::Padding);
    n->padding = edges;
    return with_children(arena, n, {child});
}

LayoutNode* SizedBox(LayoutArena& arena, SizeSpec width, SizeSpec height,
                     LayoutNode* child) {
    LayoutNode* n = make_node(arena, child ? NodeKind::ZStack : NodeKind::Leaf);
    n->width = width;
    n->height = height;
    if (child) with_children(arena, n, {child});
    return n;
}

LayoutNode* Spacer(LayoutArena& arena, float weight) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    n->width = SizeSpec::fill(weight);
    n->height = SizeSpec::fill(weight);
    return n;
}

LayoutNode* ScrollAreaV(LayoutArena& arena, ScrollState* state,
                        LayoutNode* child, SizeSpec width, SizeSpec height) {
    LayoutNode* n = make_node(arena, NodeKind::ScrollArea);
    n->scroll = state;
    n->width = width;
    n->height = height;
    n->draw_fn = [](LayoutNode& node, LayoutFrame& frame) {
        draw_scrollbar(node, frame);
    };
    return with_children(arena, n, {child});
}

}  // namespace looks::ui
