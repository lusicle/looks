#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/text.h"

namespace looks::ui {

namespace {

Theme make_night() {
    Theme t;
    t.window_bg = Color::hex(0x0A0B0D);
    t.panel_bg = Color::hex(0x101114);
    t.control_bg = Color::hex(0x17181C);
    t.control_bg_hover = Color::hex(0x1F2126);
    t.control_bg_active = Color::hex(0x0D0E10);
    t.hairline = Color::hex(0x212329);
    t.text_dim = Color::hex(0x8E949B);
    t.text_disabled = Color::hex(0x53565C);
    return t;
}

Theme make_ember() {
    Theme t;   // graphite surfaces, warm accent
    t.accent = Color::hex(0xE0954A);
    t.accent_dim = Color::hex(0x8A5C2E);
    return t;
}

Theme make_paper() {
    Theme t;
    t.window_bg = Color::hex(0xDCDAD5);
    t.panel_bg = Color::hex(0xEDEBE6);
    t.control_bg = Color::hex(0xE0DDD7);
    t.control_bg_hover = Color::hex(0xD4D1CA);
    t.control_bg_active = Color::hex(0xC8C5BE);
    t.accent = Color::hex(0x3B6EA8);
    t.accent_dim = Color::hex(0x8FA9C6);
    t.hairline = Color::hex(0xC2BFB8);
    t.text = Color::hex(0x232426);
    t.text_dim = Color::hex(0x63656A);
    t.text_disabled = Color::hex(0xA5A7AB);
    return t;
}

int g_theme_index = 0;

}  // namespace

const Theme& default_theme() { return theme_preset(0); }

int theme_count() { return 4; }

const char* theme_name(int index) {
    static const char* kNames[] = {"graphite", "night", "ember", "paper"};
    return kNames[((index % 4) + 4) % 4];
}

const Theme& theme_preset(int index) {
    static const Theme presets[] = {Theme{}, make_night(), make_ember(),
                                    make_paper()};
    return presets[((index % 4) + 4) % 4];
}

const Theme& active_theme() { return theme_preset(g_theme_index); }

void set_active_theme(int index) {
    g_theme_index = ((index % theme_count()) + theme_count()) % theme_count();
}

int active_theme_index() { return g_theme_index; }

namespace {

constexpr float kTransitionSeconds = 0.12f;

float transition_step(float current, bool on, float dt) {
    const float target = on ? 1.0f : 0.0f;
    const float rate = std::min(1.0f, dt / kTransitionSeconds);
    return current + (target - current) * rate;
}

// Press-then-release-inside click contract shared by button-like widgets.
// Returns true on a completed click this frame.
bool tick_press_release(ButtonState& state, WidgetId id, const Rect& rect,
                        LayoutFrame& frame) {
    const bool owns = frame.ctx.widget_owns_mouse(id);
    bool clicked = false;
    if (frame.input.left_pressed() && owns && !state.pressed) {
        state.pressed = true;
        frame.ctx.set_capture(id);
    }
    if (state.pressed && frame.input.left_released()) {
        if (rect.contains(frame.input.mouse)) clicked = true;
        state.pressed = false;
        frame.ctx.clear_capture();
    }
    const bool hovered = owns;
    state.hover_t = transition_step(state.hover_t, hovered && !state.pressed,
                                    frame.dt);
    state.press_t = transition_step(state.press_t, state.pressed, frame.dt);
    state.hover_seconds =
        hovered && !state.pressed ? state.hover_seconds + frame.dt : 0.0f;
    return clicked;
}

// Arms the deferred tooltip after a sustained hover.
void maybe_tooltip(const ButtonState& state, const char* tooltip,
                   LayoutFrame& frame) {
    if (tooltip && state.hover_seconds > 0.5f)
        frame.ctx.set_tooltip(tooltip,
                              {frame.input.mouse.x + 12.0f,
                               frame.input.mouse.y + 18.0f});
}

void register_rect_hit(LayoutNode& node, LayoutFrame& frame, const void* state) {
    Rect r = node.rect;
    if (!node.clip.empty()) r = r.intersect(node.clip);
    frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(state));
}

// ---- Label

struct LabelUser {
    const char* text;
    size_t length;
    float size;
    Color color;
    bool header;
};

const Font& label_font(const LabelUser& u, const LayoutFrame& frame) {
    return u.header && frame.header_font ? *frame.header_font : frame.font;
}

Vec2 measure_label(LayoutNode& node, const Constraints&,
                   const LayoutFrame& frame) {
    const auto* u = static_cast<const LabelUser*>(node.user);
    return measure_text(label_font(*u, frame), {u->text, u->length}, u->size);
}

void draw_label(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const LabelUser*>(node.user);
    const Font& font = label_font(*u, frame);
    // Center vertically when the arranged rect is taller than the text —
    // labels sit on the same line as neighbouring controls in rows.
    const float text_h = font.line_height() * u->size;
    const float inner_h = node.rect.h - node.padding.t - node.padding.b;
    const float y_off = std::max(0.0f, (inner_h - text_h) * 0.5f);
    draw_text(frame.canvas, font, {u->text, u->length},
              {node.rect.x + node.padding.l,
               node.rect.y + node.padding.t + y_off},
              u->size, u->color);
}

// ---- Button

struct ButtonUser {
    const char* label;
    size_t length;
    ButtonState* state;
    bool* out_clicked;
    bool disabled;
    bool flat;
    bool align_left;
    const char* tooltip;
};

Vec2 measure_button(LayoutNode& node, const Constraints&,
                    const LayoutFrame& frame) {
    const auto* u = static_cast<const ButtonUser*>(node.user);
    const float text_w =
        measure_text(frame.font, {u->label, u->length}, frame.theme.font_size).x;
    if (u->flat) return {std::max(kMicroSize, text_w + 8.0f), kMicroSize + 4.0f};
    return {std::max(64.0f, text_w + 24.0f), frame.theme.control_height};
}

void hit_button(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ButtonUser*>(node.user);
    if (!u->disabled) register_rect_hit(node, frame, u->state);
}

void draw_button(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ButtonUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    Color bg = theme.control_bg;
    Color fg = u->flat ? theme.text_dim : theme.text;
    if (u->disabled) {
        fg = theme.text_disabled;
    } else {
        const WidgetId id = frame.ctx.acquire_widget_id(u->state);
        if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
            *u->out_clicked = true;
        maybe_tooltip(*u->state, u->tooltip, frame);
        bg = lerp(bg, theme.control_bg_hover, u->state->hover_t);
        bg = lerp(bg, theme.control_bg_active, u->state->press_t);
        if (u->flat) fg = lerp(theme.text_dim, theme.text, u->state->hover_t);
    }

    if (u->flat) {
        // Chrome fades in with hover; resting state is just the label.
        if (!u->disabled && u->state->hover_t > 0.01f) {
            Color hover_bg = theme.control_bg_hover;
            hover_bg.a *= u->state->hover_t;
            frame.canvas.draw_sdf_rect(r, theme.corner_radius, hover_bg);
        }
    } else {
        frame.canvas.draw_sdf_rect(r, theme.corner_radius, bg);
        frame.canvas.draw_sdf_rect_outline(r, theme.corner_radius,
                                           theme.stroke_width, theme.hairline);
    }

    // Label clipped to the button: a label wider than its slot truncates
    // instead of bleeding into neighbours (narrow Fill rows).
    const Rect text_clip = node.clip.empty() ? r : r.intersect(node.clip);
    frame.canvas.push_clip(text_clip);
    const Vec2 text_size =
        measure_text(frame.font, {u->label, u->length}, theme.font_size);
    const float tx = u->align_left
        ? r.x + 4.0f
        : r.x + std::max(4.0f, (r.w - text_size.x) * 0.5f);
    draw_text(frame.canvas, frame.font, {u->label, u->length},
              {tx,
               r.y + (r.h - frame.font.line_height() * theme.font_size) * 0.5f},
              theme.font_size, fg);
    frame.canvas.pop_clip();
}

// ---- Chip (toggle)

struct ChipUser {
    const char* label;
    size_t length;
    bool on;
    ButtonState* state;
    bool* out_clicked;
    const char* tooltip;
};

Vec2 measure_chip(LayoutNode& node, const Constraints&,
                  const LayoutFrame& frame) {
    const auto* u = static_cast<const ChipUser*>(node.user);
    const float text_w = measure_text(frame.font, {u->label, u->length},
                                      frame.theme.font_size_small).x;
    return {text_w + 16.0f, frame.theme.control_height};
}

void hit_chip(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ChipUser*>(node.user);
    register_rect_hit(node, frame, u->state);
}

void draw_chip(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ChipUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    const WidgetId id = frame.ctx.acquire_widget_id(u->state);
    if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
        *u->out_clicked = true;
    maybe_tooltip(*u->state, u->tooltip, frame);

    if (u->on) {
        frame.canvas.draw_sdf_rect(r, theme.corner_radius, theme.control_bg);
    } else if (u->state->hover_t > 0.01f) {
        Color bg = theme.control_bg_hover;
        bg.a *= u->state->hover_t;
        frame.canvas.draw_sdf_rect(r, theme.corner_radius, bg);
    }
    const Color fg =
        u->on ? theme.accent
              : lerp(theme.text_dim, theme.text, u->state->hover_t);
    const Vec2 ts = measure_text(frame.font, {u->label, u->length},
                                 theme.font_size_small);
    draw_text(frame.canvas, frame.font, {u->label, u->length},
              {r.x + (r.w - ts.x) * 0.5f,
               r.y + (r.h - frame.font.line_height() *
                                theme.font_size_small) * 0.5f},
              theme.font_size_small, fg);
}

// ---- IconButton

struct IconUser {
    Icon icon;
    ButtonState* state;
    bool* out_clicked;
    bool disabled;
    const char* tooltip;
};

Vec2 measure_icon(LayoutNode&, const Constraints&, const LayoutFrame& frame) {
    return {frame.theme.control_height + 4.0f, frame.theme.control_height};
}

void hit_icon(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const IconUser*>(node.user);
    if (!u->disabled) register_rect_hit(node, frame, u->state);
}

void draw_icon_button(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const IconUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    Color fg = theme.text_dim;
    if (u->disabled) {
        fg = theme.text_disabled;
    } else {
        const WidgetId id = frame.ctx.acquire_widget_id(u->state);
        if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
            *u->out_clicked = true;
        maybe_tooltip(*u->state, u->tooltip, frame);
        if (u->state->hover_t > 0.01f) {
            Color bg = theme.control_bg_hover;
            bg.a *= u->state->hover_t;
            frame.canvas.draw_sdf_rect(r, theme.corner_radius, bg);
        }
        fg = lerp(theme.text_dim, theme.text, u->state->hover_t);
    }

    const float cx = r.x + r.w * 0.5f;
    const float cy = r.y + r.h * 0.5f;
    // Glyph-drawn icons ride the MSDF text pipeline for clean AA.
    auto glyph = [&](const char* s) {
        const Vec2 ts = measure_text(frame.font, s, theme.font_size);
        draw_text(frame.canvas, frame.font, s,
                  {cx - ts.x * 0.5f,
                   cy - frame.font.line_height() * theme.font_size * 0.5f},
                  theme.font_size, fg);
    };
    switch (u->icon) {
        case Icon::Play:
            frame.canvas.draw_triangle({cx - 4.0f, cy - 5.5f},
                                       {cx + 5.5f, cy},
                                       {cx - 4.0f, cy + 5.5f}, fg);
            break;
        case Icon::Pause:
            // SDF rects: anti-aliased, unlike raw triangles.
            frame.canvas.draw_sdf_rect({cx - 5.0f, cy - 5.0f, 3.5f, 10.0f},
                                       1.0f, fg);
            frame.canvas.draw_sdf_rect({cx + 1.5f, cy - 5.0f, 3.5f, 10.0f},
                                       1.0f, fg);
            break;
        case Icon::Up:
            // Chevron strokes (the app's fold language), not filled
            // triangles — 1 px lines read clean, triangles alias.
            frame.canvas.draw_line({cx - 4.0f, cy + 2.0f}, {cx, cy - 2.0f},
                                   1.0f, fg);
            frame.canvas.draw_line({cx, cy - 2.0f}, {cx + 4.0f, cy + 2.0f},
                                   1.0f, fg);
            break;
        case Icon::Down:
            frame.canvas.draw_line({cx - 4.0f, cy - 2.0f}, {cx, cy + 2.0f},
                                   1.0f, fg);
            frame.canvas.draw_line({cx, cy + 2.0f}, {cx + 4.0f, cy - 2.0f},
                                   1.0f, fg);
            break;
        case Icon::Close:
            glyph("x");
            break;
        case Icon::Wave: {
            // Two-arch sine: modulation.
            const float a = 2.6f;
            frame.canvas.draw_line({cx - 5.0f, cy}, {cx - 2.5f, cy - a},
                                   1.0f, fg);
            frame.canvas.draw_line({cx - 2.5f, cy - a}, {cx, cy}, 1.0f, fg);
            frame.canvas.draw_line({cx, cy}, {cx + 2.5f, cy + a}, 1.0f, fg);
            frame.canvas.draw_line({cx + 2.5f, cy + a}, {cx + 5.0f, cy},
                                   1.0f, fg);
            break;
        }
        case Icon::Key: {
            // Keyframe diamond (outline strokes).
            const float d = 3.6f;
            frame.canvas.draw_line({cx, cy - d}, {cx + d, cy}, 1.0f, fg);
            frame.canvas.draw_line({cx + d, cy}, {cx, cy + d}, 1.0f, fg);
            frame.canvas.draw_line({cx, cy + d}, {cx - d, cy}, 1.0f, fg);
            frame.canvas.draw_line({cx - d, cy}, {cx, cy - d}, 1.0f, fg);
            break;
        }
        case Icon::Knob:
            // Macro dot: SDF circle (full-radius rect).
            frame.canvas.draw_sdf_rect({cx - 3.5f, cy - 3.5f, 7.0f, 7.0f},
                                       3.5f, fg);
            break;
        case Icon::Eye:
            // Capsule outline + pupil.
            frame.canvas.draw_sdf_rect_outline({cx - 6.0f, cy - 3.5f, 12.0f,
                                                7.0f},
                                               3.5f, 1.0f, fg);
            frame.canvas.draw_sdf_rect({cx - 1.75f, cy - 1.75f, 3.5f, 3.5f},
                                       1.75f, fg);
            break;
        case Icon::EyeOff:
            frame.canvas.draw_sdf_rect_outline({cx - 6.0f, cy - 3.5f, 12.0f,
                                                7.0f},
                                               3.5f, 1.0f, fg);
            frame.canvas.draw_line({cx - 6.0f, cy + 5.0f},
                                   {cx + 6.0f, cy - 5.0f}, 1.0f, fg);
            break;
        case Icon::Dice:
            frame.canvas.draw_sdf_rect_outline({cx - 5.0f, cy - 5.0f, 10.0f,
                                                10.0f},
                                               2.0f, 1.0f, fg);
            frame.canvas.draw_sdf_rect({cx - 3.0f, cy - 3.0f, 2.0f, 2.0f},
                                       1.0f, fg);
            frame.canvas.draw_sdf_rect({cx - 1.0f, cy - 1.0f, 2.0f, 2.0f},
                                       1.0f, fg);
            frame.canvas.draw_sdf_rect({cx + 1.0f, cy + 1.0f, 2.0f, 2.0f},
                                       1.0f, fg);
            break;
        case Icon::Link:
            // Two overlapping capsule links.
            frame.canvas.draw_sdf_rect_outline({cx - 6.5f, cy - 2.5f, 8.0f,
                                                5.0f},
                                               2.5f, 1.0f, fg);
            frame.canvas.draw_sdf_rect_outline({cx - 1.5f, cy - 2.5f, 8.0f,
                                                5.0f},
                                               2.5f, 1.0f, fg);
            break;
        case Icon::Solo:
            glyph("s");
            break;
        case Icon::SoloOn:
            // Active solo reads accent even at rest.
            fg = theme.accent;
            glyph("s");
            break;
        case Icon::Copy:
            // Two offset outline squares: duplicate.
            frame.canvas.draw_sdf_rect_outline({cx - 5.5f, cy - 5.5f, 8.0f,
                                                8.0f},
                                               1.5f, 1.0f, fg);
            frame.canvas.draw_sdf_rect_outline({cx - 2.0f, cy - 2.0f, 8.0f,
                                                8.0f},
                                               1.5f, 1.0f, fg);
            break;
    }
}

// ---- Dropdown

struct DropdownUser {
    const char* const* items;
    int count;
    int selected;
    DropdownState* state;
    int* out_selected;
    const char* tooltip;
};

constexpr float kOptionRowH = 20.0f;

Rect dropdown_popup_rect(const DropdownUser& u, const Rect& anchor,
                         const LayoutFrame& frame) {
    float widest = anchor.w;
    for (int i = 0; i < u.count; ++i)
        widest = std::max(widest,
                          measure_text(frame.font, u.items[i],
                                       frame.theme.font_size).x + 24.0f);
    const float h = static_cast<float>(u.count) * kOptionRowH + 8.0f;
    float y = anchor.bottom() + 2.0f;
    const Vec2 view = frame.canvas.viewport();
    if (y + h > view.y - 4.0f) y = anchor.y - h - 2.0f;   // flip upward
    const float x = std::max(4.0f, std::min(anchor.x, view.x - widest - 4.0f));
    return {x, y, widest, h};
}

Vec2 measure_dropdown(LayoutNode& node, const Constraints& c,
                      const LayoutFrame& frame) {
    const auto* u = static_cast<const DropdownUser*>(node.user);
    const char* current =
        u->selected >= 0 && u->selected < u->count ? u->items[u->selected]
                                                   : "-";
    const float text_w =
        measure_text(frame.font, current, frame.theme.font_size).x;
    const float w = c.bounded_w() && node.width.mode == SizeMode::Fill
        ? c.max_w : std::max(64.0f, text_w + 28.0f);
    // Closed height ALWAYS — the open list is an overlay (RunPopup), so
    // opening never reflows the surrounding layout.
    return {w, frame.theme.control_height};
}

void hit_dropdown(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const DropdownUser*>(node.user);
    register_rect_hit(node, frame, &u->state->button);
    if (u->state->open)   // the popup owns everything under it while open
        frame.ctx.add_hit(dropdown_popup_rect(*u, node.rect, frame),
                          frame.ctx.acquire_widget_id(u->state),
                          HitLayer::Popup);
}

void draw_dropdown(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const DropdownUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    DropdownState& st = *u->state;

    const WidgetId id = frame.ctx.acquire_widget_id(&st.button);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    if (tick_press_release(st.button, id, r, frame)) {
        st.open = !st.open;
        if (st.open) frame.ctx.set_popup_owner(&st);
    }
    // One open popup at a time; click-away closes (a press over the popup
    // itself is owned by the popup's hit layer, not "away").
    if (st.open && frame.ctx.popup_owner() != &st) st.open = false;
    const bool over_popup =
        st.open && frame.ctx.widget_owns_mouse(
                       frame.ctx.acquire_widget_id(u->state));
    if (st.open && !owns && !over_popup && frame.input.left_pressed())
        st.open = false;
    maybe_tooltip(st.button, u->tooltip, frame);

    Color bg = lerp(theme.control_bg, theme.control_bg_hover,
                    st.button.hover_t);
    frame.canvas.draw_sdf_rect(r, theme.corner_radius, bg);
    frame.canvas.draw_sdf_rect_outline(r, theme.corner_radius,
                                       theme.stroke_width, theme.hairline);

    const char* current =
        u->selected >= 0 && u->selected < u->count ? u->items[u->selected]
                                                   : "-";
    const Rect text_clip = node.clip.empty() ? r : r.intersect(node.clip);
    frame.canvas.push_clip(text_clip);
    draw_text(frame.canvas, frame.font, current,
              {r.x + 8.0f,
               r.y + (r.h - frame.font.line_height() * theme.font_size) *
                         0.5f},
              theme.font_size, theme.text);
    frame.canvas.pop_clip();
    // Caret: down when closed, up when open.
    const float cxr = r.right() - 11.0f;
    const float cyr = r.y + r.h * 0.5f - (st.open ? -1.5f : 1.0f);
    const float dir = st.open ? -3.5f : 3.5f;
    frame.canvas.draw_line({cxr - 3.5f, cyr}, {cxr, cyr + dir}, 1.0f,
                           theme.text_dim);
    frame.canvas.draw_line({cxr, cyr + dir}, {cxr + 3.5f, cyr}, 1.0f,
                           theme.text_dim);

    if (st.open) {
        Context::PopupRequest req;
        req.anchor = r;
        req.rect = dropdown_popup_rect(*u, r, frame);
        req.items = u->items;
        req.count = u->count;
        req.selected = u->selected;
        req.state = &st;
        req.out_selected = u->out_selected;
        frame.ctx.set_popup(req);
    }
}

// ---- Scrubber

struct ScrubberUser {
    float* frame_value;
    float frame_count;
    ScrubberState* state;
    bool* out_changed;
};

Vec2 measure_scrubber(LayoutNode&, const Constraints& c,
                      const LayoutFrame& frame) {
    return {c.bounded_w() ? c.max_w : 240.0f, frame.theme.control_height};
}

void hit_scrubber(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ScrubberUser*>(node.user);
    register_rect_hit(node, frame, u->state);
}

void draw_scrubber(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ScrubberUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    ScrubberState& s = *u->state;

    const WidgetId id = frame.ctx.acquire_widget_id(&s);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    if (frame.input.left_pressed() && owns && !s.dragging) {
        s.dragging = true;
        frame.ctx.set_capture(id);
    }
    if (s.dragging) {
        const float last = std::max(0.0f, u->frame_count - 1.0f);
        const float t = r.w > 1.0f
            ? std::clamp((frame.input.mouse.x - r.x) / r.w, 0.0f, 1.0f)
            : 0.0f;
        const float next = std::round(t * last);
        if (next != *u->frame_value) {
            *u->frame_value = next;
            if (u->out_changed) *u->out_changed = true;
        }
        if (frame.input.left_released()) {
            s.dragging = false;
            frame.ctx.clear_capture();
        }
    }

    // Thin track + elapsed fill + playhead line: transport, not a slider.
    const float track_h = 4.0f;
    const Rect track{r.x, r.y + (r.h - track_h) * 0.5f, r.w, track_h};
    const float last = std::max(1.0f, u->frame_count - 1.0f);
    const float t = std::clamp(*u->frame_value / last, 0.0f, 1.0f);
    frame.canvas.draw_sdf_rect(track, 2.0f, theme.control_bg_active);
    if (t > 0.0f)
        frame.canvas.draw_sdf_rect({track.x, track.y, track.w * t, track.h},
                                   2.0f, theme.accent_dim);
    const float px = track.x + track.w * t;
    frame.canvas.draw_rect({px - 1.0f, r.y + 3.0f, 2.0f, r.h - 6.0f},
                           theme.accent);
}

// ---- Checkbox

struct CheckboxUser {
    const char* label;
    size_t length;
    bool* value;
    ButtonState* state;
    bool* out_changed;
};

Vec2 measure_checkbox(LayoutNode& node, const Constraints&,
                      const LayoutFrame& frame) {
    const auto* u = static_cast<const CheckboxUser*>(node.user);
    const float text_w =
        measure_text(frame.font, {u->label, u->length}, frame.theme.font_size).x;
    return {14.0f + 8.0f + text_w, frame.theme.control_height};
}

void hit_checkbox(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const CheckboxUser*>(node.user);
    register_rect_hit(node, frame, u->state);
}

void draw_checkbox(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const CheckboxUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    const WidgetId id = frame.ctx.acquire_widget_id(u->state);
    if (tick_press_release(*u->state, id, r, frame)) {
        *u->value = !*u->value;
        if (u->out_changed) *u->out_changed = true;
    }

    const float box = 14.0f;
    const Rect box_rect{r.x, r.y + (r.h - box) * 0.5f, box, box};
    Color bg = lerp(theme.control_bg, theme.control_bg_hover, u->state->hover_t);
    frame.canvas.draw_sdf_rect(box_rect, 2.0f, bg);
    frame.canvas.draw_sdf_rect_outline(box_rect, 2.0f, theme.stroke_width,
                                       *u->value ? theme.accent : theme.hairline);
    if (*u->value)
        frame.canvas.draw_sdf_rect(box_rect.inset(3.5f), 1.0f, theme.accent);

    draw_text(frame.canvas, frame.font, {u->label, u->length},
              {box_rect.right() + 8.0f,
               r.y + (r.h - frame.font.line_height() * theme.font_size) * 0.5f},
              theme.font_size, theme.text);
}

// ---- Slider

struct SliderUser {
    float* value;
    float min_value;
    float max_value;
    SliderState* state;
    const char* format;
    bool* out_changed;
    bool* out_released;
};

Vec2 measure_slider(LayoutNode&, const Constraints& c, const LayoutFrame& frame) {
    const float w = c.bounded_w() ? c.max_w : 160.0f;
    return {w, frame.theme.control_height};
}

void hit_slider(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SliderUser*>(node.user);
    register_rect_hit(node, frame, u->state);
}

void draw_slider(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SliderUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    SliderState& s = *u->state;

    const WidgetId id = frame.ctx.acquire_widget_id(&s);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    if (frame.input.left_pressed() && owns && !s.dragging) {
        s.dragging = true;
        frame.ctx.set_capture(id);
    }
    if (s.dragging) {
        const float span = u->max_value - u->min_value;
        const float t = r.w > 1.0f
            ? std::clamp((frame.input.mouse.x - r.x) / r.w, 0.0f, 1.0f) : 0.0f;
        const float next = u->min_value + t * span;
        if (next != *u->value) {
            *u->value = next;
            if (u->out_changed) *u->out_changed = true;
        }
        if (frame.input.left_released()) {
            s.dragging = false;
            frame.ctx.clear_capture();
            if (u->out_released) *u->out_released = true;
        }
    }

    const float span = u->max_value - u->min_value;
    const float t = span != 0.0f
        ? std::clamp((*u->value - u->min_value) / span, 0.0f, 1.0f) : 0.0f;

    frame.canvas.draw_sdf_rect(r, theme.corner_radius, theme.control_bg_active);
    if (t > 0.0f) {
        frame.canvas.push_clip({r.x, r.y, r.w * t, r.h});
        frame.canvas.draw_sdf_rect(r, theme.corner_radius,
                                   s.dragging ? theme.accent : theme.accent_dim);
        frame.canvas.pop_clip();
    }
    frame.canvas.draw_sdf_rect_outline(r, theme.corner_radius,
                                       theme.stroke_width, theme.hairline);

    if (u->format) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), u->format, *u->value);
        const float text_w =
            measure_text(frame.font, buf, theme.font_size_small).x;
        draw_text(frame.canvas, frame.font, buf,
                  {r.right() - text_w - 6.0f,
                   r.y + (r.h - frame.font.line_height() * theme.font_size_small) * 0.5f},
                  theme.font_size_small, theme.text);
    }
}

// ---- SectionHeader

struct SectionUser {
    const char* label;
    size_t length;
    bool open;
    bool small;   // nested level: body font, indented, smaller
    ButtonState* state;
    bool* out_clicked;
};

Vec2 measure_section(LayoutNode&, const Constraints& c,
                     const LayoutFrame& frame) {
    const float w = c.bounded_w() ? c.max_w : 200.0f;
    return {w, frame.theme.control_height};
}

void hit_section(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SectionUser*>(node.user);
    register_rect_hit(node, frame, u->state);
}

void draw_section(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SectionUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    const WidgetId id = frame.ctx.acquire_widget_id(u->state);
    if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
        *u->out_clicked = true;

    const Color fg = lerp(theme.text_dim, theme.text, u->state->hover_t);
    // Chevron drawn with two strokes: ▸ folded, ▾ open (no glyph needed).
    const float cx = r.x + (u->small ? 13.0f : 5.0f);
    const float cy = r.y + r.h * 0.5f;
    if (u->open) {
        frame.canvas.draw_line({cx - 4.0f, cy - 2.0f}, {cx, cy + 2.0f}, 1.0f,
                               fg);
        frame.canvas.draw_line({cx, cy + 2.0f}, {cx + 4.0f, cy - 2.0f}, 1.0f,
                               fg);
    } else {
        frame.canvas.draw_line({cx - 2.0f, cy - 4.0f}, {cx + 2.0f, cy}, 1.0f,
                               fg);
        frame.canvas.draw_line({cx + 2.0f, cy}, {cx - 2.0f, cy + 4.0f}, 1.0f,
                               fg);
    }

    const Font& font = !u->small && frame.header_font ? *frame.header_font
                                                      : frame.font;
    const float size = u->small ? theme.font_size : theme.font_size + 4.0f;
    draw_text(frame.canvas, font, {u->label, u->length},
              {r.x + (u->small ? 24.0f : 16.0f),
               r.y + (r.h - font.line_height() * size) * 0.5f},
              size, fg);
}

// ---- Panel / Separator

struct PanelUser {
    float corner_radius;
    bool outline;
    bool accent_edge;
};

void draw_panel(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const PanelUser*>(node.user);
    const float radius =
        u->corner_radius < 0.0f ? frame.theme.corner_radius : u->corner_radius;
    frame.canvas.draw_sdf_rect(node.rect, radius, frame.theme.panel_bg);
    if (u->outline)
        frame.canvas.draw_sdf_rect_outline(node.rect, radius,
                                           frame.theme.stroke_width,
                                           frame.theme.hairline);
    if (u->accent_edge)
        frame.canvas.draw_rect({node.rect.x, node.rect.y + 2.0f, 2.0f,
                                node.rect.h - 4.0f},
                               frame.theme.accent);
}

void draw_separator(LayoutNode& node, LayoutFrame& frame) {
    const Rect& r = node.rect;
    frame.canvas.draw_rect({r.x, r.y + r.h * 0.5f, r.w, 1.0f},
                           frame.theme.hairline);
}

}  // namespace

LayoutNode* Label(LayoutArena& arena, std::string_view text,
                  const LabelOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<LabelUser>();
    u->text = arena.dup(text.data(), text.size());
    u->length = text.size();
    u->size = opts.size > 0.0f ? opts.size : active_theme().font_size;
    u->color = opts.color.a > 0.0f ? opts.color : active_theme().text;
    u->header = opts.header;
    n->user = u;
    n->measure_fn = measure_label;
    n->draw_fn = draw_label;
    n->debug_name = "label";
    return n;
}

LayoutNode* Heading(LayoutArena& arena, std::string_view text) {
    LabelOpts opts;
    opts.size = active_theme().font_size_heading;
    opts.header = true;   // serif display face when one is loaded
    return Label(arena, text, opts);
}

LayoutNode* Button(LayoutArena& arena, std::string_view label,
                   ButtonState* state, bool* out_clicked,
                   const ButtonOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<ButtonUser>();
    u->label = arena.dup(label.data(), label.size());
    u->length = label.size();
    u->state = state;
    u->out_clicked = out_clicked;
    u->disabled = opts.disabled;
    u->flat = opts.flat;
    u->align_left = opts.align_left;
    u->tooltip = opts.tooltip;
    n->user = u;
    n->width = opts.width;
    n->measure_fn = measure_button;
    n->draw_fn = draw_button;
    n->hit_fn = hit_button;
    n->debug_name = "button";
    return n;
}

LayoutNode* Chip(LayoutArena& arena, std::string_view label, bool on,
                 ButtonState* state, bool* out_clicked, const char* tooltip) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<ChipUser>();
    u->label = arena.dup(label.data(), label.size());
    u->length = label.size();
    u->on = on;
    u->state = state;
    u->out_clicked = out_clicked;
    u->tooltip = tooltip;
    n->user = u;
    n->measure_fn = measure_chip;
    n->draw_fn = draw_chip;
    n->hit_fn = hit_chip;
    n->debug_name = "chip";
    return n;
}

LayoutNode* IconButton(LayoutArena& arena, Icon icon, ButtonState* state,
                       bool* out_clicked, const ButtonOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<IconUser>();
    u->icon = icon;
    u->state = state;
    u->out_clicked = out_clicked;
    u->disabled = opts.disabled;
    u->tooltip = opts.tooltip;
    n->user = u;
    n->width = opts.width;
    n->measure_fn = measure_icon;
    n->draw_fn = draw_icon_button;
    n->hit_fn = hit_icon;
    n->debug_name = "icon_button";
    return n;
}

LayoutNode* Dropdown(LayoutArena& arena, const char* const* items, int count,
                     int selected, DropdownState* state, int* out_selected,
                     SizeSpec width, const char* tooltip) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<DropdownUser>();
    u->items = items;
    u->count = count;
    u->selected = selected;
    u->state = state;
    u->out_selected = out_selected;
    u->tooltip = tooltip;
    n->user = u;
    n->width = width;
    n->measure_fn = measure_dropdown;
    n->draw_fn = draw_dropdown;
    n->hit_fn = hit_dropdown;
    n->debug_name = "dropdown";
    return n;
}

void RunPopup(Canvas2D& canvas, const Font& font, const Theme& theme,
              Context& ctx, UiInput& input) {
    if (!ctx.has_popup()) return;
    const Context::PopupRequest req = ctx.popup();
    ctx.clear_popup();
    auto* st = static_cast<DropdownState*>(req.state);
    if (!st || !st->open) return;

    const Rect& r = req.rect;
    canvas.draw_sdf_rect(r, theme.corner_radius, theme.control_bg);
    canvas.draw_sdf_rect_outline(r, theme.corner_radius, theme.stroke_width,
                                 theme.hairline);
    for (int i = 0; i < req.count; ++i) {
        const Rect ir{r.x + 4.0f,
                      r.y + 4.0f + static_cast<float>(i) * 20.0f,
                      r.w - 8.0f, 20.0f};
        const bool hover = ir.contains(input.mouse);
        if (hover) canvas.draw_sdf_rect(ir, 2.0f, theme.control_bg_hover);
        const Color fg = i == req.selected
            ? theme.accent
            : (hover ? theme.text : theme.text_dim);
        draw_text(canvas, font, req.items[i],
                  {ir.x + 6.0f,
                   ir.y + (ir.h - font.line_height() * theme.font_size) *
                              0.5f},
                  theme.font_size, fg);
        if (hover && input.left_pressed()) {
            if (req.out_selected) *req.out_selected = i;
            st->open = false;
        }
    }
}

LayoutNode* Scrubber(LayoutArena& arena, float* frame, float frame_count,
                     ScrubberState* state, bool* out_changed) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<ScrubberUser>();
    u->frame_value = frame;
    u->frame_count = frame_count;
    u->state = state;
    u->out_changed = out_changed;
    n->user = u;
    n->width = SizeSpec::fill();
    n->measure_fn = measure_scrubber;
    n->draw_fn = draw_scrubber;
    n->hit_fn = hit_scrubber;
    n->debug_name = "scrubber";
    return n;
}

LayoutNode* Checkbox(LayoutArena& arena, std::string_view label, bool* value,
                     ButtonState* state, bool* out_changed) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<CheckboxUser>();
    u->label = arena.dup(label.data(), label.size());
    u->length = label.size();
    u->value = value;
    u->state = state;
    u->out_changed = out_changed;
    n->user = u;
    n->measure_fn = measure_checkbox;
    n->draw_fn = draw_checkbox;
    n->hit_fn = hit_checkbox;
    n->debug_name = "checkbox";
    return n;
}

LayoutNode* SliderF(LayoutArena& arena, float* value, float min_value,
                    float max_value, SliderState* state,
                    const SliderOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<SliderUser>();
    u->value = value;
    u->min_value = min_value;
    u->max_value = max_value;
    u->state = state;
    u->format = opts.format;
    u->out_changed = opts.out_changed;
    u->out_released = opts.out_released;
    n->user = u;
    n->width = SizeSpec::fill();
    n->measure_fn = measure_slider;
    n->draw_fn = draw_slider;
    n->hit_fn = hit_slider;
    n->debug_name = "slider";
    return n;
}

LayoutNode* SectionHeader(LayoutArena& arena, std::string_view text,
                          bool open, ButtonState* state, bool* out_clicked,
                          bool small) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<SectionUser>();
    u->label = arena.dup(text.data(), text.size());
    u->length = text.size();
    u->open = open;
    u->small = small;
    u->state = state;
    u->out_clicked = out_clicked;
    n->user = u;
    n->width = SizeSpec::fill();
    n->measure_fn = measure_section;
    n->draw_fn = draw_section;
    n->hit_fn = hit_section;
    n->debug_name = "section";
    return n;
}

LayoutNode* Panel(LayoutArena& arena, LayoutNode* child, const PanelOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Padding);
    n->padding = opts.padding;
    auto* u = arena.alloc<PanelUser>();
    u->corner_radius = opts.corner_radius;
    u->outline = opts.outline;
    u->accent_edge = opts.accent_edge;
    n->user = u;
    n->draw_fn = draw_panel;
    n->debug_name = "panel";
    return with_children(arena, n, {child});
}

LayoutNode* Separator(LayoutArena& arena) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    n->width = SizeSpec::fill();
    n->height = SizeSpec::fixed(9.0f);
    n->draw_fn = draw_separator;
    n->debug_name = "separator";
    return n;
}

}  // namespace looks::ui
