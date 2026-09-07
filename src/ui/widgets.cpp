#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ui/probe.h"
#include "ui/text.h"

namespace looks::ui {

void register_rect_hit(LayoutNode& node, LayoutFrame& frame,
                       const void* state) {
    Rect r = node.rect;
    if (!node.clip.empty()) r = r.intersect(node.clip);
    frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(state));
}

Rect list_popup_rect(const Rect& anchor, float min_w,
                     const char* const* items, int count, const Font& font,
                     float font_size, Vec2 viewport) {
    float widest = min_w;
    for (int i = 0; i < count; ++i)
        widest = std::max(widest,
                          measure_text(font, items[i], font_size).x + 24.0f);
    const float h = static_cast<float>(count) * kPopupRowH + 8.0f;
    float y = anchor.bottom() + 2.0f;
    if (y + h > viewport.y - 4.0f) y = anchor.y - h - 2.0f;
    const float x =
        std::max(4.0f, std::min(anchor.x, viewport.x - widest - 4.0f));
    return {x, y, widest, h};
}

Rect list_popup_rect(const Rect& anchor, float min_w,
                     const char* const* items, int count,
                     const LayoutFrame& frame) {
    return list_popup_rect(anchor, min_w, items, count, frame.font,
                           frame.theme.font_size, frame.canvas.viewport());
}

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
    Theme t;
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

LayoutNode* FormRow(LayoutArena& arena, std::string_view label,
                    LayoutNode* value, LayoutNode* actions,
                    const LabelOpts& opts) {
    const Theme& theme = active_theme();
    LabelOpts text = opts;
    if (text.color.a == 0) text.color = theme.text_dim;
    text.wrap = true;
    StackOpts row;
    row.gap = theme.panel_gap;
    row.cross_align = AlignMode::Center;
    row.width = SizeSpec::fill();
    return HStack(arena, row,
        {SizedBox(arena, SizeSpec::fixed(theme.form_label_width),
                  {}, Label(arena, label, text)),
         SizedBox(arena, SizeSpec::fill(), {}, value), actions});
}

LayoutNode* PanelPage(LayoutArena& arena, const PanelContent& page) {
    const Theme& theme = active_theme();
    StackOpts column;
    column.gap = theme.panel_gap;
    column.width = SizeSpec::fill();
    column.height = SizeSpec::fill();
    LayoutNode* content = ScrollAreaV(arena, page.scroll, page.body);
    if (page.inset_body) {
        PanelOpts well;
        well.padding = Edges::all(kSpaceTight);
        well.outline = false;
        well.bg = theme.well_bg();
        content = Panel(arena, content, well);
        content->height = SizeSpec::fill();
    }
    PanelOpts panel;
    panel.padding = Edges::all(theme.panel_padding);
    const std::string probe = "panel:" + std::string(page.title);
    panel.probe = arena.dup(probe.data(), probe.size());
    LayoutNode* result = Panel(arena, VStack(arena, column,
        {Heading(arena, page.title), page.toolbar, content, page.footer}), panel);
    if (page.out_panel) *page.out_panel = result;
    return result;
}

LayoutNode* TabContainer(LayoutArena& arena, int selected,
                         std::initializer_list<PanelTab> tabs) {
    return TabContainer(arena, selected, tabs.begin(), tabs.size());
}

LayoutNode* TabContainer(LayoutArena& arena, int selected,
                         const PanelTab* tabs, size_t count) {
    StackOpts column;
    column.gap = kSpaceTight;
    column.width = SizeSpec::fill();
    column.height = SizeSpec::fill();
    StackOpts strip;
    strip.gap = kSpaceTight;
    strip.width = SizeSpec::fill();
    strip.cross_align = AlignMode::Center;
    std::vector<LayoutNode*> buttons;
    buttons.reserve(count);
    LayoutNode* page = nullptr;
    int index = 0;
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(count) - 1));
    for (size_t i = 0; i < count; ++i) {
        const PanelTab& tab = tabs[i];
        if (tab.content.out_panel) *tab.content.out_panel = nullptr;
        const bool active = index++ == selected;
        buttons.push_back(Chip(arena, tab.label, active, tab.state,
                                tab.clicked, tab.tooltip ? tab.tooltip
                                    : arena.dup(tab.label.data(), tab.label.size())));
        if (active) page = PanelPage(arena, tab.content);
    }
    return VStack(arena, column,
        {WrapDyn(arena, strip, buttons),
         SizedBox(arena, SizeSpec::fill(), SizeSpec::fill(), page)});
}

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

const Color* chip_palette() {
    static const Color palette[5] = {
        Color::hex(0xC9A23F), Color::hex(0x3FA7A0), Color::hex(0x8A6FD1),
        Color::hex(0x5B84B1), Color::hex(0xC96A8F),
    };
    return palette;
}

const Color* category_palette() {
    static const Color palette[8] = {
        Color::hex(0x7E57C2), Color::hex(0x26A69A), Color::hex(0xEC7063),
        Color::hex(0x5DADE2), Color::hex(0xF5B041), Color::hex(0xA1887F),
        Color::hex(0x66BB6A), Color::hex(0xB0BEC5),
    };
    return palette;
}

float transition_step(float current, bool on, float dt) {
    constexpr float kTransitionSeconds = 0.12f;
    const float target = on ? 1.0f : 0.0f;
    const float rate = std::min(1.0f, dt / kTransitionSeconds);
    return current + (target - current) * rate;
}

void draw_icon_glyph(Canvas2D& canvas, const Font& font, Icon icon,
                     Vec2 center, Color color, float font_size) {
    const float cx = center.x;
    const float cy = center.y;
    auto glyph = [&](const char* s) {
        const Vec2 ts = measure_text(font, s, font_size);
        draw_text(canvas, font, s,
                  {cx - ts.x * 0.5f,
                   cy - font.line_height() * font_size * 0.5f},
                  font_size, color);
    };
    switch (icon) {
        case Icon::Play:
            canvas.draw_triangle({cx - 4.0f, cy - 5.5f}, {cx + 5.5f, cy},
                                 {cx - 4.0f, cy + 5.5f}, color);
            break;
        case Icon::Pause:
            canvas.draw_sdf_rect({cx - 5.0f, cy - 5.0f, 3.5f, 10.0f}, 1.0f,
                                 color);
            canvas.draw_sdf_rect({cx + 1.5f, cy - 5.0f, 3.5f, 10.0f}, 1.0f,
                                 color);
            break;
        case Icon::Up:
            canvas.draw_line({cx - 4.0f, cy + 2.0f}, {cx, cy - 2.0f}, 1.0f,
                             color);
            canvas.draw_line({cx, cy - 2.0f}, {cx + 4.0f, cy + 2.0f}, 1.0f,
                             color);
            break;
        case Icon::Down:
            canvas.draw_line({cx - 4.0f, cy - 2.0f}, {cx, cy + 2.0f}, 1.0f,
                             color);
            canvas.draw_line({cx, cy + 2.0f}, {cx + 4.0f, cy - 2.0f}, 1.0f,
                             color);
            break;
        case Icon::Close:
            canvas.draw_line({cx - 4.0f, cy - 4.0f}, {cx + 4.0f, cy + 4.0f}, 1.2f, color);
            canvas.draw_line({cx + 4.0f, cy - 4.0f}, {cx - 4.0f, cy + 4.0f}, 1.2f, color);
            break;
        case Icon::Wave: {
            const float a = 2.6f;
            canvas.draw_line({cx - 5.0f, cy}, {cx - 2.5f, cy - a}, 1.0f,
                             color);
            canvas.draw_line({cx - 2.5f, cy - a}, {cx, cy}, 1.0f, color);
            canvas.draw_line({cx, cy}, {cx + 2.5f, cy + a}, 1.0f, color);
            canvas.draw_line({cx + 2.5f, cy + a}, {cx + 5.0f, cy}, 1.0f,
                             color);
            break;
        }
        case Icon::Key: {
            const float d = 3.6f;
            canvas.draw_line({cx, cy - d}, {cx + d, cy}, 1.0f, color);
            canvas.draw_line({cx + d, cy}, {cx, cy + d}, 1.0f, color);
            canvas.draw_line({cx, cy + d}, {cx - d, cy}, 1.0f, color);
            canvas.draw_line({cx - d, cy}, {cx, cy - d}, 1.0f, color);
            break;
        }
        case Icon::Knob:
            canvas.draw_sdf_rect({cx - 3.5f, cy - 3.5f, 7.0f, 7.0f}, 3.5f,
                                 color);
            break;
        case Icon::Eye:
            canvas.draw_sdf_rect_outline({cx - 6.0f, cy - 3.5f, 12.0f, 7.0f},
                                         3.5f, 1.0f, color);
            canvas.draw_sdf_rect({cx - 1.75f, cy - 1.75f, 3.5f, 3.5f}, 1.75f,
                                 color);
            break;
        case Icon::EyeOff:
            canvas.draw_sdf_rect_outline({cx - 6.0f, cy - 3.5f, 12.0f, 7.0f},
                                         3.5f, 1.0f, color);
            canvas.draw_line({cx - 6.0f, cy + 5.0f}, {cx + 6.0f, cy - 5.0f},
                             1.0f, color);
            break;
        case Icon::Dice:
            canvas.draw_sdf_rect_outline({cx - 5.0f, cy - 5.0f, 10.0f, 10.0f},
                                         2.0f, 1.0f, color);
            canvas.draw_sdf_rect({cx - 3.0f, cy - 3.0f, 2.0f, 2.0f}, 1.0f,
                                 color);
            canvas.draw_sdf_rect({cx - 1.0f, cy - 1.0f, 2.0f, 2.0f}, 1.0f,
                                 color);
            canvas.draw_sdf_rect({cx + 1.0f, cy + 1.0f, 2.0f, 2.0f}, 1.0f,
                                 color);
            break;
        case Icon::Link:
            canvas.draw_sdf_rect_outline({cx - 6.5f, cy - 2.5f, 8.0f, 5.0f},
                                         2.5f, 1.0f, color);
            canvas.draw_sdf_rect_outline({cx - 1.5f, cy - 2.5f, 8.0f, 5.0f},
                                         2.5f, 1.0f, color);
            break;
        case Icon::Solo:
        case Icon::SoloOn:
            glyph("s");
            break;
        case Icon::Lock:
            canvas.draw_sdf_rect({cx - 4.0f, cy - 0.5f, 8.0f, 6.0f}, 1.5f,
                                 color);
            canvas.draw_line({cx - 2.5f, cy - 0.5f}, {cx - 2.5f, cy - 3.5f},
                             1.0f, color);
            canvas.draw_line({cx + 2.5f, cy - 0.5f}, {cx + 2.5f, cy - 3.5f},
                             1.0f, color);
            canvas.draw_line({cx - 2.5f, cy - 3.5f}, {cx + 2.5f, cy - 3.5f},
                             1.0f, color);
            break;
        case Icon::Magnet:
            canvas.draw_line({cx - 3.5f, cy - 5.0f}, {cx - 3.5f, cy + 1.5f},
                             1.5f, color);
            canvas.draw_line({cx + 3.5f, cy - 5.0f}, {cx + 3.5f, cy + 1.5f},
                             1.5f, color);
            canvas.draw_line({cx - 3.5f, cy + 1.5f}, {cx - 1.5f, cy + 4.0f},
                             1.5f, color);
            canvas.draw_line({cx + 3.5f, cy + 1.5f}, {cx + 1.5f, cy + 4.0f},
                             1.5f, color);
            canvas.draw_line({cx - 1.5f, cy + 4.0f}, {cx + 1.5f, cy + 4.0f},
                             1.5f, color);
            canvas.draw_sdf_rect({cx - 4.5f, cy - 5.5f, 2.0f, 2.0f}, 0.5f,
                                 color);
            canvas.draw_sdf_rect({cx + 2.5f, cy - 5.5f, 2.0f, 2.0f}, 0.5f,
                                 color);
            break;
        case Icon::Copy:
            canvas.draw_sdf_rect_outline({cx - 5.5f, cy - 5.5f, 8.0f, 8.0f},
                                         1.5f, 1.0f, color);
            canvas.draw_sdf_rect_outline({cx - 2.0f, cy - 2.0f, 8.0f, 8.0f},
                                         1.5f, 1.0f, color);
            break;
        case Icon::ChevronRight:
            canvas.draw_line({cx - 1.5f, cy - 3.5f}, {cx + 2.0f, cy}, 1.2f,
                             color);
            canvas.draw_line({cx + 2.0f, cy}, {cx - 1.5f, cy + 3.5f}, 1.2f,
                             color);
            break;
        case Icon::Save:
            canvas.draw_line({cx - 5.0f, cy - 5.0f}, {cx + 2.0f, cy - 5.0f}, 1.0f, color);
            canvas.draw_line({cx + 2.0f, cy - 5.0f}, {cx + 5.0f, cy - 2.0f}, 1.0f, color);
            canvas.draw_line({cx + 5.0f, cy - 2.0f}, {cx + 5.0f, cy + 5.0f}, 1.0f, color);
            canvas.draw_line({cx + 5.0f, cy + 5.0f}, {cx - 5.0f, cy + 5.0f}, 1.0f, color);
            canvas.draw_line({cx - 5.0f, cy + 5.0f}, {cx - 5.0f, cy - 5.0f}, 1.0f, color);
            break;
    }
}

namespace {

// Returns true when a press releases inside the rect.
bool tick_press_release(ButtonState& state, WidgetId id, const Rect& rect,
                        LayoutFrame& frame) {
    const Gesture g = frame.ctx.gesture(id, rect);
    if (g.pressed) state.pressed = true;
    if (g.drag_released) state.pressed = false;
    state.hover_t = transition_step(state.hover_t,
                                    g.hovered && !state.pressed, frame.dt);
    state.press_t = transition_step(state.press_t, state.pressed, frame.dt);
    state.hover_seconds =
        g.hovered && !state.pressed ? state.hover_seconds + frame.dt : 0.0f;
    return g.clicked;
}

void maybe_tooltip(const ButtonState& state, const char* tooltip,
                   LayoutFrame& frame) {
    if (tooltip && state.hover_seconds > 0.5f)
        frame.ctx.set_tooltip(tooltip,
                              {frame.input.mouse.x + 12.0f,
                               frame.input.mouse.y + 18.0f});
}

struct LabelUser {
    const char* text;
    size_t length;
    float size;
    Color color;
    bool header;
    bool wrap;
};

const Font& label_font(const LabelUser& u, const LayoutFrame& frame) {
    return u.header && frame.header_font ? *frame.header_font : frame.font;
}

Vec2 measure_label(LayoutNode& node, const Constraints& c,
                   const LayoutFrame& frame) {
    const auto* u = static_cast<const LabelUser*>(node.user);
    if (u->wrap) return measure_text_wrapped(label_font(*u, frame),
        {u->text, u->length}, u->size, c.max_w);
    return measure_text(label_font(*u, frame), {u->text, u->length}, u->size);
}

void draw_label(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const LabelUser*>(node.user);
    const Font& font = label_font(*u, frame);
    const float text_h = font.line_height() * u->size;
    const float inner_h = node.rect.h - node.padding.t - node.padding.b;
    const float y_off = std::max(0.0f, (inner_h - text_h) * 0.5f);
    if (node.rect.empty()) return;
    frame.canvas.push_clip(node.rect);
    if (u->wrap)
        draw_text_wrapped(frame.canvas, font, {u->text, u->length},
            {node.rect.x + node.padding.l, node.rect.y + node.padding.t},
            u->size, node.rect.w - node.padding.l - node.padding.r, u->color);
    else
    draw_text(frame.canvas, font, {u->text, u->length},
              {node.rect.x + node.padding.l,
               node.rect.y + node.padding.t + y_off},
              u->size, u->color);
    frame.canvas.pop_clip();
}

struct ButtonUser {
    bool primary;
    const char* label;
    size_t length;
    ButtonState* state;
    bool* out_clicked;
    bool disabled;
    bool flat;
    bool align_left;
    bool active;
    const char* tooltip;
    bool* out_ctx;
    const char* probe;
    bool* out_hovered;
    int trailing_icon;
};

Vec2 measure_button(LayoutNode& node, const Constraints&,
                    const LayoutFrame& frame) {
    const auto* u = static_cast<const ButtonUser*>(node.user);
    const float text_w =
        measure_text(frame.font, {u->label, u->length}, frame.theme.font_size).x;
    if (u->flat) return {std::max(kMicroSize, text_w + 8.0f), kMicroSize + 4.0f};
    return {std::max(64.0f, text_w + 24.0f), frame.theme.control_height};
}

template <class User>
void hit_state(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const User*>(node.user);
    register_rect_hit(node, frame, u->state);
}

template <class User>
void hit_enabled(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const User*>(node.user);
    if (!u->disabled) register_rect_hit(node, frame, u->state);
}

void draw_hover(LayoutFrame& frame, const Rect& r, float radius,
                float hover_t) {
    if (hover_t <= 0.01f) return;
    Color bg = frame.theme.control_bg_hover;
    bg.a *= hover_t;
    frame.canvas.draw_sdf_rect(r, radius, bg);
}

}  // namespace

void draw_button_face(Canvas2D& canvas, const Font& font, const Theme& theme,
                      const Rect& r, std::string_view label,
                      const ButtonFace& f, const Rect& clip) {
    Color bg = lerp(theme.control_bg, theme.control_bg_hover, f.hover_t);
    bg = lerp(bg, theme.control_bg_active, f.press_t);
    Color fg = f.flat ? lerp(theme.text_dim, theme.text, f.hover_t)
                      : theme.text;
    if (f.disabled) fg = theme.text_disabled;
    if (f.active && !f.disabled) fg = theme.accent;
    if (f.primary && !f.disabled) {
        bg = lerp(theme.accent, theme.accent_dim, f.press_t);
        const float luminance = 0.2126f * bg.r + 0.7152f * bg.g + 0.0722f * bg.b;
        fg = luminance > 0.179f ? Color{0, 0, 0, 1} : Color{1, 1, 1, 1};
    }
    if (f.flat) {
        if (!f.disabled && f.hover_t > 0.01f)
            canvas.draw_sdf_rect(r, f.radius,
                                 theme.control_bg_hover.with_alpha(
                                     theme.control_bg_hover.a * f.hover_t));
    } else {
        canvas.draw_sdf_rect(r, f.radius, bg);
        canvas.draw_sdf_rect_outline(
            r, f.radius, theme.stroke_width,
            (f.active || f.primary) && !f.disabled ? theme.accent : theme.hairline);
    }
    const float icon_w = f.trailing_icon >= 0 ? 14.0f * f.scale : 0.0f;
    const Rect text_r{r.x, r.y, std::max(0.0f, r.w - icon_w), r.h};
    // Clip the label so a wide label truncates instead of bleeding out.
    const Rect text_clip = clip.empty() ? text_r : text_r.intersect(clip);
    canvas.push_clip(text_clip);
    const Vec2 text_size = measure_text(font, label, f.font_size);
    const float pad = 4.0f * f.scale;
    const float tx = f.align_left
        ? r.x + (f.flat ? pad : theme.control_text_inset * f.scale)
        : r.x + std::max(pad, (text_r.w - text_size.x) * 0.5f);
    draw_text(canvas, font, label,
              {tx, r.y + (r.h - font.line_height() * f.font_size) * 0.5f},
              f.font_size, fg);
    canvas.pop_clip();
    if (f.trailing_icon >= 0)
        draw_icon_glyph(canvas, font, static_cast<Icon>(f.trailing_icon),
                        {r.right() - 8.0f * f.scale, r.y + r.h * 0.5f},
                        f.disabled ? theme.text_disabled : theme.text_dim,
                        f.font_size);
}

float value_box_width(const Font& font, std::string_view value,
                      float font_size, float scale) {
    return std::max(38.0f * scale,
                    measure_text(font, value, font_size).x +
                    2.0f * active_theme().control_text_inset * scale);
}

Rect slider_value_rect(const Rect& r, float box_w) {
    return {r.right() - box_w, r.y, box_w, r.h};
}

Rect slider_track_rect(const Rect& r, float box_w, float scale) {
    const float gap = box_w > 0.0f ? 8.0f * scale : 0.0f;
    return {r.x, r.y, std::max(0.0f, r.w - box_w - gap), r.h};
}

void draw_slider_track(Canvas2D& canvas, const Theme& theme, const Rect& track,
                       float t, float live_t, bool active, float scale) {
    const float sy = track.y + track.h * 0.5f;
    canvas.draw_rect({track.x, sy - 1.0f, track.w, 2.0f},
                     theme.control_bg_hover);
    canvas.draw_rect({track.x, sy - 1.0f, track.w * std::clamp(t, 0.0f, 1.0f),
                      2.0f},
                     theme.accent_dim);
    if (live_t >= 0.0f) {
        const float lx = track.x + track.w * std::clamp(live_t, 0.0f, 1.0f);
        canvas.draw_rect({lx - 1.0f, sy - 5.0f * scale,
                          std::max(2.0f, 1.5f * scale), 10.0f * scale},
                         theme.accent);
    }
    const float hx = track.x + track.w * std::clamp(t, 0.0f, 1.0f);
    canvas.draw_sdf_rect({hx - 2.5f * scale, sy - 4.0f * scale, 5.0f * scale,
                          8.0f * scale},
                         1.5f * scale, active ? theme.accent : theme.text);
}

bool caret_blink_on(const Context& ctx) { return (ctx.frame() / 30) % 2 == 0; }

TextCaret field_caret(const TextField& f, int offset, bool blink_on) {
    TextCaret c;
    c.caret = blink_on ? offset + f.caret : -1;
    c.sel_lo = offset + f.sel_lo();
    c.sel_hi = offset + f.sel_hi();
    return c;
}

void draw_dial_face(Canvas2D& canvas, const Theme& theme, Vec2 center,
                    float knob_radius, float deg, bool active, float live_deg,
                    bool has_live, float scale) {
    const Rect knob{center.x - knob_radius, center.y - knob_radius,
                    knob_radius * 2.0f, knob_radius * 2.0f};
    canvas.draw_sdf_rect(knob, knob_radius, theme.control_bg_active);
    canvas.draw_sdf_rect_outline(knob, knob_radius, theme.stroke_width,
                                 theme.hairline);
    canvas.draw_line({center.x, knob.y - 3.0f * scale},
                     {center.x, knob.y - 1.0f * scale}, 1.0f, theme.text_dim);
    const auto needle = [&](float d, float len, Color c, float w) {
        const float rad = std::fmod(d, 360.0f) * 0.0174533f;
        canvas.draw_line(
            {center.x, center.y},
            {center.x + std::sin(rad) * (knob_radius - 2.0f * scale) * len,
             center.y - std::cos(rad) * (knob_radius - 2.0f * scale) * len},
            w, c);
    };
    if (has_live) needle(live_deg, 1.0f, theme.accent, 1.5f * scale);
    needle(deg, 1.0f, active ? theme.accent : theme.text, 1.5f * scale);
}

void draw_dropdown_face(Canvas2D& canvas, const Font& font, const Theme& theme,
                        const Rect& r, std::string_view text, bool open,
                        float hover_t, float font_size, float radius,
                        float scale, const Rect& clip) {
    canvas.draw_sdf_rect(
        r, radius, lerp(theme.control_bg, theme.control_bg_hover, hover_t));
    canvas.draw_sdf_rect_outline(r, radius, theme.stroke_width,
                                 open ? theme.accent : theme.hairline);
    // The gutter holds the chevron; text stops before it, never under it.
    Rect text_r = r;
    text_r.w = std::max(0.0f, text_r.w - 19.0f * scale);
    const Rect text_clip = clip.empty() ? text_r : text_r.intersect(clip);
    canvas.push_clip(text_clip);
    draw_text(canvas, font, text,
              {r.x + theme.control_text_inset * scale,
               r.y + (r.h - font.line_height() * font_size) * 0.5f},
              font_size, theme.text);
    canvas.pop_clip();
    const float cxr = r.right() - 11.0f * scale;
    const float cyr = r.y + r.h * 0.5f - (open ? -1.5f : 1.0f) * scale;
    const float cs = 3.5f * scale;
    const float dir = open ? -cs : cs;
    canvas.draw_line({cxr - cs, cyr}, {cxr, cyr + dir}, 1.0f, theme.text_dim);
    canvas.draw_line({cxr, cyr + dir}, {cxr + cs, cyr}, 1.0f, theme.text_dim);
}

void draw_text_input_face(Canvas2D& canvas, const Font& font,
                          const Theme& theme, const Rect& r,
                          std::string_view text, const TextCaret& caret,
                          bool focused, bool flat, bool dim, float hover_t,
                          float font_size, float radius, float scale,
                          const Rect& clip) {
    if (flat) {
        const float t = std::max(hover_t, focused ? 1.0f : 0.0f);
        if (t > 0.01f)
            canvas.draw_sdf_rect(r, radius,
                                 theme.control_bg_hover.with_alpha(
                                     theme.control_bg_hover.a * t));
    } else {
        canvas.draw_sdf_rect(
            r, radius, lerp(theme.control_bg, theme.control_bg_hover, hover_t));
        canvas.draw_sdf_rect_outline(r, radius, theme.stroke_width,
                                     focused ? theme.accent : theme.hairline);
    }
    const Rect text_clip = clip.empty() ? r : r.intersect(clip);
    canvas.push_clip(text_clip);
    const float tx = r.x + theme.control_text_inset * scale;
    const float ty = r.y + (r.h - font.line_height() * font_size) * 0.5f;
    auto x_at = [&](int index) {
        const int n = std::clamp(index, 0, static_cast<int>(text.size()));
        return tx +
               measure_text(font, text.substr(0, static_cast<size_t>(n)),
                            font_size)
                   .x;
    };
    const float bar_y = r.y + 3.0f * scale;
    const float bar_h = std::max(1.0f, r.h - 6.0f * scale);
    if (focused && caret.sel_lo < caret.sel_hi) {
        const float x0 = x_at(caret.sel_lo);
        const float x1 = x_at(caret.sel_hi);
        canvas.draw_rect({x0, bar_y, x1 - x0, bar_h},
                         theme.accent_dim.with_alpha(0.5f));
    }
    draw_text(canvas, font, text, {tx, ty}, font_size,
              dim ? theme.text_dim : theme.text);
    if (focused && caret.caret >= 0)
        canvas.draw_rect({x_at(caret.caret), bar_y, std::max(1.0f, scale),
                          bar_h},
                         theme.accent);
    canvas.pop_clip();
}

void draw_popup_chrome(Canvas2D& canvas, const Theme& theme, const Rect& r) {
    canvas.draw_sdf_rect(r, theme.corner_radius, theme.control_bg);
    canvas.draw_sdf_rect_outline(r, theme.corner_radius, theme.stroke_width,
                                 theme.hairline);
}

void draw_popup_row(Canvas2D& canvas, const Theme& theme, const Rect& r,
                    bool hover) {
    if (hover) canvas.draw_sdf_rect(r, 2.0f, theme.control_bg_hover);
}

TextResult text_input_keys(TextField& field, UiInput& input, TextNav* nav) {
    TextResult last = TextResult::None;
    for (const platform::Event& e : input.keys) {
        if (nav && e.type == platform::Event::Type::KeyDown) {
            if (e.key == platform::Key::Up) {
                nav->up = true;
                continue;
            }
            if (e.key == platform::Key::Down) {
                nav->down = true;
                continue;
            }
            if (e.key == platform::Key::Tab) {
                nav->tab = true;
                continue;
            }
        }
        if (nav && e.type == platform::Event::Type::Char &&
            e.codepoint == '\t')
            continue;
        const TextResult r = text_field_key(field, e);
        if (r != TextResult::None) last = r;
        if (r == TextResult::Commit || r == TextResult::Cancel) break;
    }
    return last;
}

bool text_input_focused(const Context& ctx, const TextInputState* state) {
    return ctx.has_focus(const_cast<Context&>(ctx).acquire_widget_id(state));
}

void text_input_focus(Context& ctx, const TextInputState* state) {
    ctx.set_focus(ctx.acquire_widget_id(state));
}

TextHostResult text_host_frame(LayoutFrame& frame, WidgetId id,
                               TextInputState& st, TextField& field,
                               const Rect& r, bool grab_focus,
                               const TextHostOuts& outs) {
    TextHostResult res;
    bool focused = frame.ctx.has_focus(id);
    if (grab_focus && !focused && !st.had_focus &&
        frame.ctx.focus().is_null()) {
        frame.ctx.set_focus(id);
        focused = true;
    }
    if (tick_press_release(st.button, id, r, frame)) {
        if (!focused) {
            frame.ctx.set_focus(id);
            focused = true;
        }
        if (outs.clicked) *outs.clicked = true;
    }
    if (focused && frame.input.left_pressed() &&
        !r.contains(frame.input.mouse)) {
        frame.ctx.clear_focus();
        focused = false;
    }
    if (focused) {
        const TextResult k = text_input_keys(field, frame.input, outs.nav);
        if (k == TextResult::Edit && outs.changed) *outs.changed = true;
        if (k == TextResult::Commit || k == TextResult::Cancel) {
            if (k == TextResult::Commit && outs.commit) *outs.commit = true;
            if (k == TextResult::Cancel && outs.cancel) *outs.cancel = true;
            frame.ctx.clear_focus();
            focused = false;
            res.ended = true;
        }
    }
    if (st.had_focus && !focused && !res.ended && outs.blur) *outs.blur = true;
    st.had_focus = focused;
    res.focused = focused;
    return res;
}

static bool parse_number(const TextField& field, double& value,
                         float scale, float offset) {
    char* end = nullptr;
    const double typed = std::strtod(field.buf.c_str(), &end);
    const double next = (typed - offset) / (scale != 0.0f ? scale : 1.0f);
    if (end == field.buf.c_str() || *end != '\0' || !std::isfinite(next))
        return false;
    value = next;
    return true;
}

void begin_slider_edit(LayoutFrame& frame, SliderState& state,
                        float value, const SliderOpts& opts) {
    char seed[32];
    const float scale = opts.display_scale != 0.0f ? opts.display_scale : 1.0f;
    std::snprintf(seed, sizeof(seed), "%.9g", value * scale + opts.display_offset);
    state.edit.set(seed);
    state.edit.sel_anchor = 0;
    state.edit.filter = TextFilter::Signed;
    state.edit.cap = 32;
    state.edit_state = {};
    state.edit_state.had_focus = true;
    state.editing = true;
    frame.ctx.set_focus(frame.ctx.acquire_widget_id(&state.edit_state));
}

TextHostResult slider_edit_frame(LayoutFrame& frame, SliderState& state,
                                  const Rect& box, float& value,
                                  float min_value, float max_value,
                                  const SliderOpts& opts) {
    if (!state.editing) return {};
    bool commit = false, cancel = false, blur = false;
    TextHostOuts outs;
    outs.commit = &commit;
    outs.cancel = &cancel;
    outs.blur = &blur;
    const TextHostResult result = text_host_frame(
        frame, frame.ctx.acquire_widget_id(&state.edit_state),
        state.edit_state, state.edit, box, false, outs);
    if (commit || cancel || blur) {
        state.editing = false;
        double next = 0.0;
        if (!cancel && parse_number(state.edit, next, opts.display_scale,
                                    opts.display_offset)) {
            const float next_value = static_cast<float>(std::clamp(
                next, double(min_value), double(opts.hard_max != 0.0f
                    ? std::max(max_value, opts.hard_max) : max_value)));
            if (next_value != value) {
                value = next_value;
                if (opts.out_changed) *opts.out_changed = true;
            }
            if (opts.out_released) *opts.out_released = true;
        }
    }
    return result;
}

void slider_input_frame(const UiInput& input, const Gesture& gesture,
                         const Rect& track, Vec2 center, bool dial,
                         float& value, float min_value, float max_value,
                         SliderState& state, const SliderOpts& opts) {
    if (!gesture.pressed && !state.dragging) return;
    const float angle = dial
        ? std::atan2(input.mouse.x - center.x, center.y - input.mouse.y) *
              57.29578f
        : 0.0f;
    if (gesture.pressed) {
        state.dragging = true;
        state.fine = false;
        state.dial_angle = angle;
        state.fine_anchor_value = value;
    }
    const bool fine = (input.mods & platform::kModShift) != 0;
    const float scale = opts.display_scale != 0.0f ? opts.display_scale : 1.0f;
    float next;
    if (dial) {
        float delta = angle - state.dial_angle;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        state.dial_angle = angle;
        if (fine) delta *= 0.1f;
        state.fine_anchor_value = std::clamp(
            state.fine_anchor_value + delta / scale, min_value, max_value);
        next = state.fine_anchor_value;
    } else {
        if (fine != state.fine) {
            state.fine = fine;
            state.fine_anchor_value = value;
            state.fine_anchor_x = input.mouse.x;
        }
        const float span = max_value - min_value;
        if (fine) {
            next = state.fine_anchor_value +
                   (track.w > 1.0f
                        ? (input.mouse.x - state.fine_anchor_x) / track.w
                        : 0.0f) * span * 0.1f;
        } else {
            const float t = track.w > 1.0f
                ? std::clamp((input.mouse.x - track.x) / track.w, 0.0f, 1.0f)
                : 0.0f;
            next = min_value + t * span;
        }
        next = std::clamp(next, min_value, max_value);
    }
    next = std::clamp(
        (snap_to_format(next * scale + opts.display_offset, opts.format) -
         opts.display_offset) / scale, min_value, max_value);
    if (next != value) {
        value = next;
        if (opts.out_changed) *opts.out_changed = true;
    }
    if (gesture.drag_released) {
        state.dragging = false;
        state.fine = false;
        if (opts.out_released) *opts.out_released = true;
    }
}

namespace {

void draw_button(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ButtonUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    if (!u->disabled)
        probe_add(u->probe ? std::string(u->probe)
                           : std::string(u->label, u->length),
                  r);

    ButtonFace face;
    if (!u->disabled) {
        const WidgetId id = frame.ctx.acquire_widget_id(u->state);
        if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
            *u->out_clicked = true;
        const bool over = frame.ctx.widget_owns_mouse(id) &&
                          r.contains(frame.input.mouse);
        if (u->out_hovered && over) *u->out_hovered = true;
        if (u->out_ctx && frame.input.right_pressed() && over)
            *u->out_ctx = true;
        maybe_tooltip(*u->state, u->tooltip, frame);
        face.hover_t = u->state->hover_t;
        face.press_t = u->state->press_t;
    }
    face.active = u->active;
    face.primary = u->primary;
    face.disabled = u->disabled;
    face.flat = u->flat;
    face.align_left = u->align_left;
    face.font_size = theme.font_size;
    face.radius = theme.corner_radius;
    face.trailing_icon = u->trailing_icon;
    draw_button_face(frame.canvas, frame.font, theme, r,
                     {u->label, u->length}, face, node.clip);
}

struct TextInputUser {
    TextField* field;
    TextInputState* state;
    const char* placeholder;
    const char* prefix;
    const char* text;
    const char* probe;
    const char* tooltip;
    bool flat;
    bool small;
    bool grab_focus;
    bool* out_clicked;
    bool* out_changed;
    bool* out_commit;
    bool* out_cancel;
    bool* out_blur;
    TextNav* nav;
};

Vec2 measure_text_input(LayoutNode& node, const Constraints&,
                        const LayoutFrame& frame) {
    const auto* u = static_cast<const TextInputUser*>(node.user);
    const float fs =
        u->small ? frame.theme.font_size_small : frame.theme.font_size;
    const float text_w = measure_text(frame.font, u->field->buf, fs).x;
    return {std::max(64.0f, text_w + 24.0f), frame.theme.control_height};
}

void draw_text_input(LayoutNode& node, LayoutFrame& frame) {
    auto* u = static_cast<TextInputUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    TextInputState& st = *u->state;
    const WidgetId id = frame.ctx.acquire_widget_id(&st);
    probe_add(u->probe         ? std::string(u->probe)
              : u->placeholder ? std::string(u->placeholder)
              : u->prefix      ? std::string(u->prefix)
                               : std::string("field"),
              r);

    TextHostOuts outs;
    outs.clicked = u->out_clicked;
    outs.changed = u->out_changed;
    outs.commit = u->out_commit;
    outs.cancel = u->out_cancel;
    outs.blur = u->out_blur;
    outs.nav = u->nav;
    const TextHostResult h =
        text_host_frame(frame, id, st, *u->field, r, u->grab_focus, outs);
    maybe_tooltip(st.button, u->tooltip, frame);

    const float fs = u->small ? theme.font_size_small : theme.font_size;
    std::string shown = u->prefix ? u->prefix : "";
    const int prefix_len = static_cast<int>(shown.size());
    bool dim = false;
    TextCaret caret;
    if (h.focused) {
        shown += u->field->buf;
        caret = field_caret(*u->field, prefix_len, caret_blink_on(frame.ctx));
    } else if (u->text) {
        shown += u->text;
    } else if (!u->field->buf.empty()) {
        shown += u->field->buf;
    } else {
        if (u->placeholder) shown += u->placeholder;
        dim = true;
    }
    draw_text_input_face(frame.canvas, frame.font, theme, r, shown, caret,
                         h.focused, u->flat, dim, st.button.hover_t, fs,
                         theme.corner_radius, 1.0f, node.clip);
}

struct SegmentedUser {
    const char* const* labels;
    const char* const* tooltips;
    int count;
    int active;
    ButtonState* states;
    bool* const* out_clicked;
};

Rect segment_rect(const Rect& r, int i, int count) {
    const float w = r.w / static_cast<float>(count);
    return {r.x + w * static_cast<float>(i), r.y, w, r.h};
}

Vec2 measure_segmented(LayoutNode& node, const Constraints&,
                       const LayoutFrame& frame) {
    const auto* u = static_cast<const SegmentedUser*>(node.user);
    float widest = 0.0f;
    for (int i = 0; i < u->count; ++i)
        widest = std::max(widest,
                          measure_text(frame.font, u->labels[i],
                                       frame.theme.font_size).x);
    return {(widest + 20.0f) * static_cast<float>(u->count),
            frame.theme.control_height};
}

void hit_segmented(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SegmentedUser*>(node.user);
    for (int i = 0; i < u->count; ++i) {
        Rect r = segment_rect(node.rect, i, u->count);
        if (!node.clip.empty()) r = r.intersect(node.clip);
        frame.ctx.add_hit(r, frame.ctx.acquire_widget_id(&u->states[i]));
    }
}

void draw_segmented(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SegmentedUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    const Color track = theme.control_bg;
    const Color lit_fill = theme.selection_bg();
    frame.canvas.draw_sdf_rect(r, theme.corner_radius, track);

    for (int i = 0; i < u->count; ++i) {
        const Rect seg = segment_rect(r, i, u->count);
        probe_add(u->labels[i], seg);
        ButtonState& st = u->states[i];
        const WidgetId id = frame.ctx.acquire_widget_id(&st);
        if (tick_press_release(st, id, seg, frame) && u->out_clicked[i])
            *u->out_clicked[i] = true;
        maybe_tooltip(st, u->tooltips ? u->tooltips[i] : nullptr, frame);

        const bool lit = i == u->active;
        const float seg_radius = std::max(0.0f, theme.corner_radius - 1.0f);
        if (lit) {
            frame.canvas.draw_sdf_rect(seg.inset(1.5f), seg_radius,
                                       lit_fill);
        } else {
            draw_hover(frame, seg.inset(1.5f), seg_radius, st.hover_t);
        }
        const Color fg = lit ? theme.accent : theme.text_dim;
        const Vec2 ts = measure_text(frame.font, u->labels[i],
                                     theme.font_size);
        draw_text(frame.canvas, frame.font, u->labels[i],
                  {seg.x + (seg.w - ts.x) * 0.5f,
                   seg.y +
                       (seg.h - frame.font.line_height() * theme.font_size) *
                           0.5f},
                  theme.font_size, fg);
        if (i > 0)
            frame.canvas.draw_rect(
                {seg.x, seg.y + 3.0f, 1.0f, seg.h - 6.0f}, theme.hairline);
    }
    frame.canvas.draw_sdf_rect_outline(r, theme.corner_radius,
                                       theme.stroke_width, theme.hairline);
}

struct CategoryUser {
    const char* const* labels;
    const char* const* probes;
    int count;
    int active;
    ButtonState* states;
    bool* const* out_clicked;
};

Rect category_rect(const LayoutNode& node, const Theme& theme, int index) {
    const float height = theme.control_height + theme.panel_gap;
    return {node.rect.x, node.rect.y + index * height, node.rect.w, height};
}

Vec2 measure_categories(LayoutNode& node, const Constraints&, const LayoutFrame& frame) {
    const auto& u = *static_cast<const CategoryUser*>(node.user);
    float width = 0;
    for (int i = 0; i < u.count; ++i)
        width = std::max(width, measure_text(frame.font, u.labels[i], frame.theme.font_size).x);
    return {width + frame.theme.control_text_inset * 2,
            u.count * (frame.theme.control_height + frame.theme.panel_gap)};
}

void hit_categories(LayoutNode& node, LayoutFrame& frame) {
    const auto& u = *static_cast<const CategoryUser*>(node.user);
    for (int i = 0; i < u.count; ++i) {
        Rect row = category_rect(node, frame.theme, i);
        if (!node.clip.empty()) row = row.intersect(node.clip);
        frame.ctx.add_hit(row, frame.ctx.acquire_widget_id(&u.states[i]));
    }
}

void draw_categories(LayoutNode& node, LayoutFrame& frame) {
    const auto& u = *static_cast<const CategoryUser*>(node.user);
    const Theme& theme = frame.theme;
    int focus = -1;
    for (int i = 0; i < u.count; ++i) {
        const Rect row = category_rect(node, theme, i);
        const WidgetId id = frame.ctx.acquire_widget_id(&u.states[i]);
        const bool clicked = tick_press_release(u.states[i], id, row, frame);
        if (u.states[i].pressed) frame.ctx.set_focus(id);
        if (clicked && u.out_clicked[i]) *u.out_clicked[i] = true;
        if (frame.ctx.has_focus(id)) focus = i;
        probe_add(u.probes && u.probes[i] ? u.probes[i] : u.labels[i], row);
        maybe_tooltip(u.states[i], u.labels[i], frame);
        if (i == u.active) {
            frame.canvas.draw_rect(row, theme.selection_bg());
            frame.canvas.draw_rect({row.x, row.y, theme.stroke_width * 2, row.h}, theme.accent);
        } else if (u.states[i].hover_t > 0) {
            frame.canvas.draw_rect(row, theme.control_bg_hover.with_alpha(u.states[i].hover_t));
        }
        frame.canvas.draw_rect({row.x, row.bottom() - theme.stroke_width, row.w, theme.stroke_width}, theme.hairline);
        frame.canvas.push_clip(row);
        draw_text(frame.canvas, frame.font, u.labels[i],
            {row.x + theme.control_text_inset,
             row.y + (row.h - frame.font.line_height() * theme.font_size) * 0.5f},
            theme.font_size, i == u.active ? theme.text : theme.text_dim);
        frame.canvas.pop_clip();
    }
    if (focus < 0) return;
    bool activate = false;
    for (auto it = frame.input.keys.begin(); it != frame.input.keys.end();) {
        if (it->type != platform::Event::Type::KeyDown) { ++it; continue; }
        const auto key = it->key;
        if (key == platform::Key::Up) focus = std::max(0, focus - 1);
        else if (key == platform::Key::Down) focus = std::min(u.count - 1, focus + 1);
        else if (key == platform::Key::Home) focus = 0;
        else if (key == platform::Key::End) focus = u.count - 1;
        else if (key != platform::Key::Enter && key != platform::Key::Space) { ++it; continue; }
        activate = true;
        it = frame.input.keys.erase(it);
    }
    if (activate) {
        frame.ctx.set_focus(frame.ctx.acquire_widget_id(&u.states[focus]));
        if (u.out_clicked[focus]) *u.out_clicked[focus] = true;
    }
}

struct ChipUser {
    const char* label;
    size_t length;
    bool on;
    ButtonState* state;
    bool* out_clicked;
    const char* tooltip;
    // Null gives a plain chip with no close zone.
    ButtonState* close_state;
    bool* out_close;
};

constexpr float kChipCloseW = 16.0f;

Rect chip_close_rect(const Rect& r) {
    return {r.right() - kChipCloseW, r.y, kChipCloseW, r.h};
}

Vec2 measure_chip(LayoutNode& node, const Constraints&,
                  const LayoutFrame& frame) {
    const auto* u = static_cast<const ChipUser*>(node.user);
    const float text_w = measure_text(frame.font, {u->label, u->length},
                                      frame.theme.font_size_small).x;
    return {text_w + 16.0f + (u->close_state ? kChipCloseW : 0.0f),
            frame.theme.control_height};
}

void hit_chip(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ChipUser*>(node.user);
    register_rect_hit(node, frame, u->state);
    // The close zone registers AFTER the body so it wins their overlap.
    if (u->close_state)
        frame.ctx.add_hit(chip_close_rect(node.rect),
                          frame.ctx.acquire_widget_id(u->close_state));
}

void draw_chip(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ChipUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    probe_add(std::string(u->label, u->length), r);

    const WidgetId id = frame.ctx.acquire_widget_id(u->state);
    if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
        *u->out_clicked = true;
    maybe_tooltip(*u->state, u->tooltip, frame);

    if (u->on) {
        frame.canvas.draw_sdf_rect(r, theme.corner_radius, theme.control_bg);
    } else {
        draw_hover(frame, r, theme.corner_radius, u->state->hover_t);
    }
    const Color fg =
        u->on ? theme.accent
              : lerp(theme.text_dim, theme.text, u->state->hover_t);
    const float label_w =
        r.w - (u->close_state ? kChipCloseW : 0.0f);
    const Vec2 ts = measure_text(frame.font, {u->label, u->length},
                                 theme.font_size_small);
    frame.canvas.push_clip({r.x + kSpaceTight, r.y,
                           std::max(0.0f, label_w - kSpaceTight * 2), r.h});
    draw_text(frame.canvas, frame.font, {u->label, u->length},
              {r.x + std::max(kSpaceTight, (label_w - ts.x) * 0.5f),
               r.y + (r.h - frame.font.line_height() *
                                theme.font_size_small) * 0.5f},
              theme.font_size_small, fg);
    frame.canvas.pop_clip();
    if (u->close_state) {
        const Rect cr = chip_close_rect(r);
        const WidgetId cid = frame.ctx.acquire_widget_id(u->close_state);
        if (tick_press_release(*u->close_state, cid, cr, frame) &&
            u->out_close)
            *u->out_close = true;
        maybe_tooltip(*u->close_state, "close tab", frame);
        draw_icon_glyph(frame.canvas, frame.font, Icon::Close,
                        {cr.x + cr.w * 0.5f - 1.0f, cr.y + cr.h * 0.5f},
                        lerp(theme.text_disabled, theme.text,
                             u->close_state->hover_t),
                        theme.font_size_small);
    }
}

struct IconUser {
    bool framed;
    Icon icon;
    ButtonState* state;
    bool* out_clicked;
    bool disabled;
    bool active;
    const char* tooltip;
    const char* probe;
};

Vec2 measure_icon(LayoutNode&, const Constraints&, const LayoutFrame& frame) {
    return {frame.theme.control_height + 4.0f, frame.theme.control_height};
}

void draw_icon_button(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const IconUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    if (!u->disabled) {
        static const char* names[] = {
            "play", "pause", "up", "down", "close", "wave", "key", "knob",
            "eye", "eyeoff", "dice", "link", "solo", "soloon", "copy",
            "lock", "magnet", "chevronright", "save"};
        const size_t ii = static_cast<size_t>(u->icon);
        if (u->probe)
            probe_add(u->probe, r);
        else if (ii < sizeof(names) / sizeof(names[0]))
            probe_add(std::string("icon:") + names[ii], r);
    }

    Color fg = theme.text_dim;
    if (u->framed) {
        ButtonFace face;
        face.disabled = u->disabled;
        face.active = u->active;
        face.hover_t = u->state->hover_t;
        face.press_t = u->state->press_t;
        face.radius = theme.corner_radius;
        draw_button_face(frame.canvas, frame.font, theme, r, "", face, node.clip);
    }
    if (u->disabled) {
        fg = theme.text_disabled;
    } else {
        const WidgetId id = frame.ctx.acquire_widget_id(u->state);
        if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
            *u->out_clicked = true;
        maybe_tooltip(*u->state, u->tooltip, frame);
        if (!u->framed) draw_hover(frame, r, theme.corner_radius, u->state->hover_t);
        fg = u->active
            ? theme.accent
            : lerp(theme.text_dim, theme.text, u->state->hover_t);
    }

    if (u->icon == Icon::SoloOn && !u->disabled) fg = theme.accent;
    draw_icon_glyph(frame.canvas, frame.font, u->icon,
                    {r.x + r.w * 0.5f, r.y + r.h * 0.5f}, fg,
                    theme.font_size);
}

struct DropdownUser {
    const char* const* items;
    int count;
    int selected;
    DropdownState* state;
    int* out_selected;
    const char* tooltip;
};

Rect dropdown_popup_rect(const DropdownUser& u, const Rect& anchor,
                         const LayoutFrame& frame) {
    return list_popup_rect(anchor, anchor.w, u.items, u.count, frame);
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
    // Always the closed height; the open list is an overlay.
    return {w, frame.theme.control_height};
}

void hit_dropdown(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const DropdownUser*>(node.user);
    register_rect_hit(node, frame, &u->state->button);
    if (u->state->open)   // the popup owns everything under it while open
        frame.ctx.push_overlay(dropdown_popup_rect(*u, node.rect, frame),
                               frame.ctx.acquire_widget_id(u->state), false);
}

void draw_dropdown(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const DropdownUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    DropdownState& st = *u->state;
    probe_add(std::string("dd:") +
                  (u->selected >= 0 && u->selected < u->count
                       ? u->items[u->selected]
                       : "-"),
              r);

    const WidgetId id = frame.ctx.acquire_widget_id(&st.button);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    if (tick_press_release(st.button, id, r, frame)) {
        st.open = !st.open;
        if (st.open) frame.ctx.set_popup_owner(&st);
    }
    // One popup opens at a time; a press outside it closes it.
    if (st.open && frame.ctx.popup_owner() != &st) st.open = false;
    const bool over_popup =
        st.open && frame.ctx.widget_owns_mouse(
                       frame.ctx.acquire_widget_id(u->state));
    if (st.open && !owns && !over_popup && frame.input.left_pressed())
        st.open = false;
    maybe_tooltip(st.button, u->tooltip, frame);

    const char* current =
        u->selected >= 0 && u->selected < u->count ? u->items[u->selected]
                                                   : "-";
    draw_dropdown_face(frame.canvas, frame.font, theme, r, current, st.open,
                       st.button.hover_t, theme.font_size,
                       theme.corner_radius, 1.0f, node.clip);

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

void draw_scrubber(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const ScrubberUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    ScrubberState& s = *u->state;

    const WidgetId id = frame.ctx.acquire_widget_id(&s);
    const Gesture g = frame.ctx.gesture(id, r);
    if (g.pressed) {
        s.dragging = true;
        s.moved = false;
        s.press_value = -1.0f;
    }
    if (s.dragging) {
        const float last = std::max(0.0f, u->frame_count - 1.0f);
        const float t = r.w > 1.0f
            ? std::clamp((frame.input.mouse.x - r.x) / r.w, 0.0f, 1.0f)
            : 0.0f;
        const float next = std::round(t * last);
        if (s.press_value < 0.0f) s.press_value = next;
        else if (std::fabs(next - s.press_value) >= 1.0f) s.moved = true;
        if (next != *u->frame_value) {
            *u->frame_value = next;
            if (u->out_changed) *u->out_changed = true;
        }
        if (g.drag_released) {
            s.dragging = false;
        }
    }

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

void draw_checkbox(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const CheckboxUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    probe_add(std::string(u->label, u->length), r);

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

struct SliderUser {
    float* value;
    float min_value;
    float max_value;
    SliderState* state;
    SliderOpts opts;
};

float slider_box_width(const SliderUser& u, const LayoutFrame& frame,
                       const char* value) {
    const bool editing = u.state->editing;
    if (!u.opts.format && !editing) return 0.0f;
    return value_box_width(frame.font,
                           editing ? std::string_view(u.state->edit.buf)
                                   : std::string_view(value),
                           frame.theme.font_size_small, 1.0f);
}

void hit_slider(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SliderUser*>(node.user);
    register_rect_hit(node, frame, u->state);
    if (u->state->editing) {
        Rect box = slider_value_rect(node.rect,
                                     slider_box_width(*u, frame, ""));
        if (!node.clip.empty()) box = box.intersect(node.clip);
        frame.ctx.add_hit(box, frame.ctx.acquire_widget_id(&u->state->edit_state));
    }
}

void draw_slider_box(const SliderUser& u, const Rect& box, const char* value,
                     LayoutFrame& frame) {
    const Theme& theme = frame.theme;
    if (u.state->editing) {
        const TextHostResult h = slider_edit_frame(
            frame, *u.state, box, *u.value, u.min_value, u.max_value, u.opts);
        draw_text_input_face(
            frame.canvas, frame.font, theme, box, u.state->edit.buf,
            h.focused ? field_caret(u.state->edit, 0, caret_blink_on(frame.ctx))
                      : TextCaret{},
            h.focused, false, false, 0.0f, theme.font_size_small,
            theme.corner_radius, 1.0f);
    } else if (u.opts.format) {
        draw_text_input_face(frame.canvas, frame.font, theme, box, value,
                             TextCaret{}, false, false, false, 0.0f,
                             theme.font_size_small, theme.corner_radius, 1.0f);
    }
}

Vec2 measure_slider(LayoutNode&, const Constraints& c, const LayoutFrame& frame) {
    const float w = c.bounded_w() ? c.max_w : 160.0f;
    return {w, frame.theme.control_height};
}

void draw_slider_control(LayoutNode& node, LayoutFrame& frame, bool dial) {
    const auto& u = *static_cast<const SliderUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    SliderState& s = *u.state;
    const float scale = u.opts.display_scale != 0.0f
        ? u.opts.display_scale : 1.0f;
    char buf[32] = {};
    if (u.opts.format)
        std::snprintf(buf, sizeof(buf), u.opts.format,
                      *u.value * scale + u.opts.display_offset);
    const float box_w = slider_box_width(u, frame, buf);
    const Rect box = slider_value_rect(r, box_w);
    const Rect track = slider_track_rect(r, box_w, 1.0f);
    const Vec2 center{r.x + 11.0f, r.y + r.h * 0.5f};
    const WidgetId id = frame.ctx.acquire_widget_id(&s);
    Gesture gesture = frame.ctx.gesture(id, r);
    if (u.opts.out_ctx && gesture.right_clicked && !s.dragging)
        *u.opts.out_ctx = true;
    if (gesture.pressed && box_w > 0.0f && box.contains(frame.input.mouse)) {
        if (!s.editing) begin_slider_edit(frame, s, *u.value, u.opts);
        gesture.pressed = false;
    }
    slider_input_frame(frame.input, gesture, track, center, dial,
                       *u.value, u.min_value, u.max_value, s, u.opts);
    s.hover_seconds = gesture.hovered && !s.dragging
        ? s.hover_seconds + frame.dt : 0.0f;
    if (u.opts.tooltip && s.hover_seconds > 0.5f)
        frame.ctx.set_tooltip(u.opts.tooltip,
                              {frame.input.mouse.x + 12.0f,
                               frame.input.mouse.y + 18.0f});
    if (dial) {
        draw_dial_face(frame.canvas, theme, center, 8.0f,
                       *u.value * scale, s.dragging, 0.0f, false, 1.0f);
    } else {
        const float span = u.max_value - u.min_value;
        const float t = span != 0.0f
            ? std::clamp((*u.value - u.min_value) / span, 0.0f, 1.0f) : 0.0f;
        draw_slider_track(frame.canvas, theme, track, t, -1.0f,
                          s.dragging, 1.0f);
    }
    draw_slider_box(u, box, buf, frame);
}

void draw_slider(LayoutNode& node, LayoutFrame& frame) {
    draw_slider_control(node, frame, false);
}

void draw_dial(LayoutNode& node, LayoutFrame& frame) {
    draw_slider_control(node, frame, true);
}

}  // namespace

void hsv_to_rgb(float h, float s, float v, float out[3]) {
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    switch (static_cast<int>(h)) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

void rgb_to_hsv(const float rgb[3], float& h, float& s, float& v) {
    const float mx = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    const float mn = std::min(rgb[0], std::min(rgb[1], rgb[2]));
    const float d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) return;   // hue keeps its previous value on gray
    if (mx == rgb[0])
        h = 60.0f * std::fmod((rgb[1] - rgb[2]) / d + 6.0f, 6.0f);
    else if (mx == rgb[1])
        h = 60.0f * ((rgb[2] - rgb[0]) / d + 2.0f);
    else
        h = 60.0f * ((rgb[0] - rgb[1]) / d + 4.0f);
}

namespace {

struct SwatchUser {
    float rgba[4];
    SwatchState* state;
    float* out_rgba;
    bool* out_changed;
    bool* out_released;
};

constexpr float kPickerW = 192.0f;
constexpr float kPickerSvH = 128.0f;
constexpr float kPickerHueH = 12.0f;
constexpr float kPickerFieldH = 17.0f;
constexpr int kSvCells = 16;
constexpr int kHueCells = 96;
constexpr float kCheckerCell = 5.0f;

float picker_height(SwatchMode mode) {
    if (mode == SwatchMode::Hue)
        return 6.0f + kPickerHueH + 6.0f + kPickerFieldH + 6.0f;
    float h = 6.0f + kPickerSvH + 6.0f + kPickerHueH + 6.0f +
              kPickerFieldH + 4.0f + kPickerFieldH + 6.0f;
    if (mode == SwatchMode::Rgba) h += kPickerHueH + 6.0f;
    return h;
}

void draw_checker(Canvas2D& canvas, const Rect& r) {
    canvas.push_clip(r);
    const Color light = Color::srgb(0.62f, 0.62f, 0.62f);
    const Color dark = Color::srgb(0.42f, 0.42f, 0.42f);
    canvas.draw_rect(r, light);
    const int cols =
        static_cast<int>(std::ceil(r.w / kCheckerCell));
    const int rows = static_cast<int>(std::ceil(r.h / kCheckerCell));
    for (int y = 0; y < rows; ++y)
        for (int x = (y & 1); x < cols; x += 2)
            canvas.draw_rect({r.x + static_cast<float>(x) * kCheckerCell,
                              r.y + static_cast<float>(y) * kCheckerCell,
                              kCheckerCell, kCheckerCell},
                             dark);
    canvas.pop_clip();
}

}  // namespace

Color hue_bar_color(float hue01, float lightness) {
    float rgb[3];
    color::hsl_to_rgb(hue01 - std::floor(hue01), 1.0f,
                      std::clamp(lightness, 0.02f, 0.98f), rgb);
    return Color::srgb(rgb[0], rgb[1], rgb[2]);
}

void swatch_seed_from(SwatchState& st, const float rgba[4]) {
    if (st.mode == SwatchMode::Hue) {
        st.hue = std::clamp(rgba[0], 0.0f, 1.0f) * 360.0f;
        st.sat = 1.0f;
        st.val = 1.0f;
        st.alpha = 1.0f;
    } else {
        rgb_to_hsv(rgba, st.hue, st.sat, st.val);
        st.alpha = st.mode == SwatchMode::Rgba
                       ? std::clamp(rgba[3], 0.0f, 1.0f)
                       : 1.0f;
    }
    st.drag_zone = 0;
    st.field_edit = -1;
    st.edit.set("");
}

void draw_swatch_face(Canvas2D& canvas, const Rect& r, float radius,
                      SwatchMode mode, const float rgba[4],
                      float hue_light) {
    if (mode == SwatchMode::Hue) {
        canvas.draw_sdf_rect(r, radius, hue_bar_color(rgba[0], hue_light));
        return;
    }
    const float a =
        mode == SwatchMode::Rgba ? std::clamp(rgba[3], 0.0f, 1.0f) : 1.0f;
    if (a < 1.0f) draw_checker(canvas, r);
    canvas.draw_sdf_rect(r, radius,
                         Color::srgb(rgba[0], rgba[1], rgba[2], a));
}

Rect swatch_popup_rect(const Rect& anchor, const SwatchState& st,
                       const LayoutFrame& frame) {
    const float h = picker_height(st.mode);
    float y = anchor.bottom() + 2.0f;
    const Vec2 view = frame.canvas.viewport();
    if (y + h > view.y - 4.0f) y = anchor.y - h - 2.0f;
    const float x =
        std::max(4.0f, std::min(anchor.x, view.x - kPickerW - 4.0f));
    return {x, y, kPickerW, h};
}

namespace {

Vec2 measure_swatch(LayoutNode&, const Constraints& c, const LayoutFrame&) {
    return {c.bounded_w() ? c.max_w : 120.0f, 14.0f};
}

void hit_swatch(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SwatchUser*>(node.user);
    register_rect_hit(node, frame, &u->state->button);
    if (u->state->open)   // the popup owns everything under it while open
        frame.ctx.push_overlay(
            swatch_popup_rect(node.rect, *u->state, frame),
            frame.ctx.acquire_widget_id(u->state), false);
}

void draw_swatch(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SwatchUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;
    SwatchState& st = *u->state;

    const WidgetId id = frame.ctx.acquire_widget_id(&st.button);
    const bool owns = frame.ctx.widget_owns_mouse(id);
    if (tick_press_release(st.button, id, r, frame)) {
        st.open = !st.open;
        if (st.open) {
            swatch_seed_from(st, u->rgba);
            frame.ctx.set_popup_owner(&st);
        }
    }
    // Closing commits the typed field first, so no value is lost.
    const auto close = [&] {
        swatch_commit_field(st, u->out_rgba, u->out_changed,
                            u->out_released);
        frame.ctx.clear_focus();
        st.open = false;
    };
    if (st.open && frame.ctx.popup_owner() != &st) close();
    const bool over_popup =
        st.open && frame.ctx.widget_owns_mouse(
                       frame.ctx.acquire_widget_id(u->state));
    if (st.open && !owns && !over_popup && frame.input.left_pressed())
        close();

    probe_add(st.mode == SwatchMode::Hue ? "swatch:hue" : "swatch:color", r);
    draw_swatch_face(frame.canvas, r, 3.0f, st.mode, u->rgba, st.hue_light);
    frame.canvas.draw_sdf_rect_outline(
        r, 3.0f, theme.stroke_width,
        lerp(theme.hairline, theme.text_dim, st.button.hover_t));

    if (st.open) {
        Context::PopupRequest req;
        req.kind = Context::PopupKind::Color;
        req.anchor = r;
        req.rect = swatch_popup_rect(r, st, frame);
        req.state = &st;
        req.out_rgb = u->out_rgba;
        req.out_changed = u->out_changed;
        req.out_released = u->out_released;
        frame.ctx.set_popup(req);
    }
}

}  // namespace

// R/G/B/A fields take 0-255 bytes. Hex takes rgb, rrggbb or rrggbbaa.
void swatch_commit_field(SwatchState& st, float* out_rgba,
                         bool* out_changed, bool* out_released) {
    if (st.field_edit < 0) return;
    const std::string& text = st.edit.buf;
    const size_t len = text.size();
    float rgb[3];
    hsv_to_rgb(st.hue, st.sat, st.val, rgb);
    float alpha = st.alpha;
    bool valid = false;
    if (st.mode == SwatchMode::Hue) {
        if (len > 0) {
            st.hue = std::clamp(static_cast<float>(std::atof(text.c_str())),
                                0.0f, 1.0f) *
                     360.0f;
            valid = true;
        }
        st.field_edit = -1;
        st.edit.set("");
        if (!valid) return;
        if (out_rgba) out_rgba[0] = st.hue / 360.0f;
        if (out_changed) *out_changed = true;
        if (out_released) *out_released = true;
        return;
    }
    if (st.field_edit < 3) {
        if (len > 0) {
            rgb[st.field_edit] =
                std::clamp(std::atoi(text.c_str()), 0, 255) / 255.0f;
            valid = true;
        }
    } else if (st.field_edit == 3) {
        if (len > 0) {
            alpha = std::clamp(std::atoi(text.c_str()), 0, 255) / 255.0f;
            valid = true;
        }
    } else if (len == 8 || len == 6 || len == 3) {
        const auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        const int pairs = len == 3 ? 3 : static_cast<int>(len) / 2;
        for (int i = 0; i < pairs; ++i) {
            const int byte =
                len == 3 ? nib(text[static_cast<size_t>(i)]) * 17
                         : nib(text[static_cast<size_t>(i) * 2]) * 16 +
                               nib(text[static_cast<size_t>(i) * 2 + 1]);
            const float v = static_cast<float>(byte) / 255.0f;
            if (i < 3)
                rgb[i] = v;
            else
                alpha = v;
        }
        valid = true;
    }
    st.field_edit = -1;
    st.edit.set("");
    if (!valid) return;
    rgb_to_hsv(rgb, st.hue, st.sat, st.val);
    if (st.mode == SwatchMode::Rgba) st.alpha = alpha;
    if (out_rgba) {
        out_rgba[0] = rgb[0];
        out_rgba[1] = rgb[1];
        out_rgba[2] = rgb[2];
        if (st.mode == SwatchMode::Rgba) out_rgba[3] = st.alpha;
    }
    if (out_changed) *out_changed = true;
    if (out_released) *out_released = true;
}

void run_color_popup(Canvas2D& canvas, const Font& font, const Theme& theme,
                     Context& ctx, const Context::PopupRequest& req,
                     UiInput& input) {
    auto* st = static_cast<SwatchState*>(req.state);
    if (!st || !st->open) return;
    const bool owns = ctx.widget_owns_mouse(ctx.acquire_widget_id(st));
    const bool hue_only = st->mode == SwatchMode::Hue;
    const bool has_alpha = st->mode == SwatchMode::Rgba;

    const Rect& r = req.rect;
    canvas.draw_sdf_rect(r, theme.corner_radius, theme.control_bg);
    canvas.draw_sdf_rect_outline(r, theme.corner_radius, theme.stroke_width,
                                 theme.hairline);
    const float inner_w = r.w - 12.0f;
    const Rect sv{r.x + 6.0f, r.y + 6.0f, inner_w,
                  hue_only ? 0.0f : kPickerSvH};
    const Rect hue{sv.x, hue_only ? r.y + 6.0f : sv.bottom() + 6.0f, sv.w,
                   kPickerHueH};
    probe_add("picker:hue", hue);
    const Rect alpha_bar{sv.x, hue.bottom() + 6.0f, sv.w,
                         has_alpha ? kPickerHueH : 0.0f};
    const float rows_y =
        (has_alpha ? alpha_bar.bottom() : hue.bottom()) + 6.0f;
    const Rect chip{sv.x, rows_y, kPickerFieldH, kPickerFieldH};

    const int byte_fields = hue_only ? 0 : (has_alpha ? 4 : 3);
    Rect fields[5];
    for (Rect& f : fields) f = {};
    if (hue_only) {
        fields[0] = {sv.x, rows_y, sv.w, kPickerFieldH};
    } else {
        const float gap = 4.0f;
        const float fw =
            (sv.w - chip.w - gap * static_cast<float>(byte_fields)) /
            static_cast<float>(byte_fields);
        for (int i = 0; i < byte_fields; ++i)
            fields[i] = {chip.right() + gap + (fw + gap) *
                                                  static_cast<float>(i),
                         chip.y, fw, kPickerFieldH};
        fields[4] = {sv.x + 14.0f, chip.bottom() + 4.0f, sv.w - 14.0f,
                     kPickerFieldH};
    }
    const int field_count = hue_only ? 1 : 5;
    const auto field_slot = [&](int i) { return hue_only ? 0 : i; };

    float cur[3];
    hsv_to_rgb(st->hue, st->sat, st->val, cur);
    if (owns && input.left_pressed()) {
        int hit_field = -1;
        for (int i = 0; i < field_count; ++i) {
            const int slot = field_slot(i);
            if (fields[slot].w > 0.0f && fields[slot].contains(input.mouse))
                hit_field = slot;
        }
        if (!hue_only && hit_field == 3 && !has_alpha) hit_field = -1;
        // A press commits the open field first, then routes to the new zone.
        if (st->field_edit >= 0 && hit_field != st->field_edit)
            swatch_commit_field(*st, req.out_rgb, req.out_changed,
                                req.out_released);
        if (hit_field >= 0 && hit_field != st->field_edit) {
            hsv_to_rgb(st->hue, st->sat, st->val, cur);
            st->field_edit = hit_field;
            char seed[16];
            if (hue_only) {
                std::snprintf(seed, sizeof(seed), "%.3f", st->hue / 360.0f);
                st->edit.filter = TextFilter::Digits;
                st->edit.cap = 5;
            } else if (hit_field < 3) {
                std::snprintf(seed, sizeof(seed), "%d",
                              static_cast<int>(cur[hit_field] * 255.0f +
                                               0.5f));
                st->edit.filter = TextFilter::Uint;
                st->edit.cap = 3;
            } else if (hit_field == 3) {
                std::snprintf(seed, sizeof(seed), "%d",
                              static_cast<int>(st->alpha * 255.0f + 0.5f));
                st->edit.filter = TextFilter::Uint;
                st->edit.cap = 3;
            } else {
                const int n = std::snprintf(
                    seed, sizeof(seed), "%02x%02x%02x",
                    static_cast<int>(cur[0] * 255.0f + 0.5f),
                    static_cast<int>(cur[1] * 255.0f + 0.5f),
                    static_cast<int>(cur[2] * 255.0f + 0.5f));
                if (has_alpha && n > 0)
                    std::snprintf(seed + n, sizeof(seed) - size_t(n), "%02x",
                                  static_cast<int>(st->alpha * 255.0f +
                                                   0.5f));
                st->edit.filter = TextFilter::Hex;
                st->edit.cap = has_alpha ? 8 : 6;
            }
            st->edit.set(seed);
            ctx.set_focus(ctx.acquire_widget_id(st));
        } else if (!hue_only && sv.contains(input.mouse)) {
            st->drag_zone = 1;
        } else if (hue.contains(input.mouse)) {
            st->drag_zone = 2;
        } else if (has_alpha && alpha_bar.contains(input.mouse)) {
            st->drag_zone = 3;
        }
    }
    if (st->field_edit >= 0 && ctx.has_focus(ctx.acquire_widget_id(st))) {
        for (const platform::Event& e : input.keys) {
            const TextResult res = text_field_key(st->edit, e);
            if (res == TextResult::Commit || res == TextResult::Cancel) {
                if (res == TextResult::Cancel) st->edit.set("");
                swatch_commit_field(*st, req.out_rgb, req.out_changed,
                                    req.out_released);
                ctx.clear_focus();
                break;
            }
        }
    }
    if (st->drag_zone != 0) {
        if (st->drag_zone == 1) {
            st->sat = std::clamp((input.mouse.x - sv.x) / sv.w, 0.0f, 1.0f);
            st->val =
                1.0f - std::clamp((input.mouse.y - sv.y) / sv.h, 0.0f, 1.0f);
        } else if (st->drag_zone == 2) {
            st->hue =
                std::clamp((input.mouse.x - hue.x) / hue.w, 0.0f, 1.0f) *
                360.0f;
        } else {
            st->alpha = std::clamp(
                (input.mouse.x - alpha_bar.x) / alpha_bar.w, 0.0f, 1.0f);
        }
        if (req.out_rgb) {
            if (hue_only) {
                req.out_rgb[0] = st->hue / 360.0f;
            } else {
                float rgb[3];
                hsv_to_rgb(st->hue, st->sat, st->val, rgb);
                req.out_rgb[0] = rgb[0];
                req.out_rgb[1] = rgb[1];
                req.out_rgb[2] = rgb[2];
                if (has_alpha) req.out_rgb[3] = st->alpha;
            }
        }
        if (req.out_changed) *req.out_changed = true;
        if (input.left_released()) {
            st->drag_zone = 0;
            if (req.out_released) *req.out_released = true;
        }
    }

    const auto axis = [](float lo, float span, int i, int n) {
        return lo + span * static_cast<float>(i) / static_cast<float>(n);
    };
    if (!hue_only) {
        const auto hsv_color = [&](float s, float v) {
            float rgb[3];
            hsv_to_rgb(st->hue, s, v, rgb);
            return Color::srgb(rgb[0], rgb[1], rgb[2]);
        };
        for (int yi = 0; yi < kSvCells; ++yi) {
            const float v0 = 1.0f - axis(0.0f, 1.0f, yi, kSvCells);
            const float v1 = 1.0f - axis(0.0f, 1.0f, yi + 1, kSvCells);
            const float y0 = axis(sv.y, sv.h, yi, kSvCells);
            const float y1 = axis(sv.y, sv.h, yi + 1, kSvCells);
            for (int xi = 0; xi < kSvCells; ++xi) {
                const float s0 = axis(0.0f, 1.0f, xi, kSvCells);
                const float s1 = axis(0.0f, 1.0f, xi + 1, kSvCells);
                const float x0 = axis(sv.x, sv.w, xi, kSvCells);
                const float x1 = axis(sv.x, sv.w, xi + 1, kSvCells);
                canvas.draw_rect_corners(
                    {x0, y0, x1 - x0, y1 - y0}, hsv_color(s0, v0),
                    hsv_color(s1, v0), hsv_color(s1, v1), hsv_color(s0, v1));
            }
        }
        const Vec2 svc{sv.x + sv.w * st->sat,
                       sv.y + sv.h * (1.0f - st->val)};
        canvas.draw_sdf_rect_outline({svc.x - 4.0f, svc.y - 4.0f, 8.0f, 8.0f},
                                     4.0f, 1.5f,
                                     st->val > 0.6f && st->sat < 0.6f
                                         ? Color{0.0f, 0.0f, 0.0f, 0.9f}
                                         : Color{1.0f, 1.0f, 1.0f, 0.9f});
    }

    const float bar_light = hue_only ? st->hue_light : 0.5f;
    for (int i = 0; i < kHueCells; ++i) {
        const Color a = hue_bar_color(
            static_cast<float>(i) / static_cast<float>(kHueCells),
            bar_light);
        const Color b = hue_bar_color(
            static_cast<float>(i + 1) / static_cast<float>(kHueCells),
            bar_light);
        const float x0 = axis(hue.x, hue.w, i, kHueCells);
        const float x1 = axis(hue.x, hue.w, i + 1, kHueCells);
        canvas.draw_rect_corners({x0, hue.y, x1 - x0, hue.h}, a, b, b, a);
    }
    const float hx = hue.x + hue.w * st->hue / 360.0f;
    canvas.draw_rect({hx - 1.0f, hue.y - 1.0f, 2.0f, hue.h + 2.0f},
                     Color{1.0f, 1.0f, 1.0f, 0.9f});

    hsv_to_rgb(st->hue, st->sat, st->val, cur);
    if (has_alpha) {
        draw_checker(canvas, alpha_bar);
        const int cells = 24;
        for (int i = 0; i < cells; ++i) {
            const float a0 =
                static_cast<float>(i) / static_cast<float>(cells);
            const float a1 =
                static_cast<float>(i + 1) / static_cast<float>(cells);
            const Color c0 = Color::srgb(cur[0], cur[1], cur[2], a0);
            const Color c1 = Color::srgb(cur[0], cur[1], cur[2], a1);
            const float x0 = axis(alpha_bar.x, alpha_bar.w, i, cells);
            const float x1 = axis(alpha_bar.x, alpha_bar.w, i + 1, cells);
            canvas.draw_rect_corners({x0, alpha_bar.y, x1 - x0, alpha_bar.h},
                                     c0, c1, c1, c0);
        }
        const float ax = alpha_bar.x + alpha_bar.w * st->alpha;
        canvas.draw_rect({ax - 1.0f, alpha_bar.y - 1.0f, 2.0f,
                          alpha_bar.h + 2.0f},
                         Color{1.0f, 1.0f, 1.0f, 0.9f});
    }

    if (!hue_only) {
        const float chip_rgba[4] = {cur[0], cur[1], cur[2], st->alpha};
        draw_swatch_face(canvas, chip, 3.0f, st->mode, chip_rgba);
        canvas.draw_sdf_rect_outline(chip, 3.0f, theme.stroke_width,
                                     theme.hairline);
        draw_text(canvas, font, "#",
                  {sv.x + 2.0f,
                   fields[4].y +
                       (fields[4].h - font.line_height() *
                                          theme.font_size_small) * 0.5f},
                  theme.font_size_small, theme.text_dim);
    }
    const int bytes[4] = {static_cast<int>(cur[0] * 255.0f + 0.5f),
                          static_cast<int>(cur[1] * 255.0f + 0.5f),
                          static_cast<int>(cur[2] * 255.0f + 0.5f),
                          static_cast<int>(st->alpha * 255.0f + 0.5f)};
    for (int i = 0; i < 5; ++i) {
        if (fields[i].w <= 0.0f) continue;
        const bool editing = st->field_edit == i;
        canvas.draw_sdf_rect(fields[i], 3.0f, theme.control_bg_active);
        canvas.draw_sdf_rect_outline(fields[i], 3.0f, theme.stroke_width,
                                     editing ? theme.accent
                                             : theme.hairline);
        char buf[16];
        std::string shown;
        if (editing) {
            shown = caret_text(st->edit);
        } else if (hue_only) {
            std::snprintf(buf, sizeof(buf), "%.3f", st->hue / 360.0f);
            shown = buf;
        } else if (i < 4) {
            std::snprintf(buf, sizeof(buf), "%d", bytes[i]);
            shown = buf;
        } else {
            const int n = std::snprintf(buf, sizeof(buf), "%02x%02x%02x",
                                        bytes[0], bytes[1], bytes[2]);
            if (has_alpha && n > 0)
                std::snprintf(buf + n, sizeof(buf) - size_t(n), "%02x",
                              bytes[3]);
            shown = buf;
        }
        draw_text(canvas, font, shown.c_str(),
                  {fields[i].x + 5.0f,
                   fields[i].y +
                       (fields[i].h - font.line_height() *
                                          theme.font_size_small) * 0.5f},
                  theme.font_size_small,
                  editing ? theme.text : theme.text_dim);
    }
}

namespace {

struct SectionUser {
    const char* label;
    size_t length;
    bool open;
    bool small;   // nested level: body font, indented
    ButtonState* state;
    bool* out_clicked;
};

Vec2 measure_section(LayoutNode&, const Constraints& c,
                     const LayoutFrame& frame) {
    const float w = c.bounded_w() ? c.max_w : 200.0f;
    return {w, frame.theme.control_height};
}

void draw_section(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const SectionUser*>(node.user);
    const Theme& theme = frame.theme;
    const Rect& r = node.rect;

    const WidgetId id = frame.ctx.acquire_widget_id(u->state);
    if (tick_press_release(*u->state, id, r, frame) && u->out_clicked)
        *u->out_clicked = true;
    probe_add(std::string("sec:") + std::string(u->label, u->length), r);
    maybe_tooltip(*u->state, u->label, frame);

    const Color fg = lerp(theme.text_dim, theme.text, u->state->hover_t);
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
    const float size = u->small ? theme.font_size : theme.font_size_heading;
    const float indent = u->small ? 24.0f : 16.0f;
    frame.canvas.push_clip({r.x + indent, r.y, std::max(0.0f, r.w - indent), r.h});
    draw_text(frame.canvas, font, {u->label, u->length},
              {r.x + (u->small ? 24.0f : 16.0f),
               r.y + (r.h - font.line_height() * size) * 0.5f},
              size, fg);
    frame.canvas.pop_clip();
}

struct PanelUser {
    float corner_radius;
    bool outline;
    bool accent_edge;
    Color bg;
    const char* probe;
};

void draw_panel(LayoutNode& node, LayoutFrame& frame) {
    const auto* u = static_cast<const PanelUser*>(node.user);
    if (u->probe) probe_add(u->probe, node.rect);
    const float radius =
        u->corner_radius < 0.0f ? frame.theme.corner_radius : u->corner_radius;
    frame.canvas.draw_sdf_rect(
        node.rect, radius, u->bg.a > 0.0f ? u->bg : frame.theme.panel_bg);
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
    u->wrap = opts.wrap;
    n->user = u;
    n->measure_fn = measure_label;
    n->draw_fn = draw_label;
    n->debug_name = "label";
    return n;
}

LayoutNode* Heading(LayoutArena& arena, std::string_view text) {
    LabelOpts opts;
    opts.size = active_theme().font_size_heading;
    opts.header = true;
    return Label(arena, text, opts);
}

LayoutNode* Button(LayoutArena& arena, std::string_view label,
                   ButtonState* state, bool* out_clicked,
                   const ButtonOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<ButtonUser>();
    u->primary = opts.primary;
    u->label = arena.dup(label.data(), label.size());
    u->length = label.size();
    u->state = state;
    u->out_clicked = out_clicked;
    u->disabled = opts.disabled;
    u->flat = opts.flat;
    u->align_left = opts.align_left;
    u->active = opts.active;
    u->tooltip = opts.tooltip;
    u->out_ctx = opts.out_ctx;
    u->probe = opts.probe;
    u->out_hovered = opts.out_hovered;
    u->trailing_icon = opts.trailing_icon;
    n->user = u;
    n->width = opts.width;
    n->measure_fn = measure_button;
    n->draw_fn = draw_button;
    n->hit_fn = hit_enabled<ButtonUser>;
    n->debug_name = "button";
    return n;
}

LayoutNode* TextInput(LayoutArena& arena, TextField* field,
                      TextInputState* state, const TextInputOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<TextInputUser>();
    u->field = field;
    u->state = state;
    u->placeholder = opts.placeholder;
    u->prefix = opts.prefix;
    u->text = opts.text;
    u->probe = opts.probe;
    u->tooltip = opts.tooltip;
    u->flat = opts.flat;
    u->small = opts.small;
    u->grab_focus = opts.grab_focus;
    u->out_clicked = opts.out_clicked;
    u->out_changed = opts.out_changed;
    u->out_commit = opts.out_commit;
    u->out_cancel = opts.out_cancel;
    u->out_blur = opts.out_blur;
    u->nav = opts.nav;
    n->user = u;
    n->width = opts.width;
    n->measure_fn = measure_text_input;
    n->draw_fn = draw_text_input;
    n->hit_fn = hit_state<TextInputUser>;
    n->debug_name = "text_input";
    return n;
}

LayoutNode* Chip(LayoutArena& arena, std::string_view label, bool on,
                 ButtonState* state, bool* out_clicked, const char* tooltip,
                 ButtonState* close_state, bool* out_close) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<ChipUser>();
    u->label = arena.dup(label.data(), label.size());
    u->length = label.size();
    u->on = on;
    u->state = state;
    u->out_clicked = out_clicked;
    u->tooltip = tooltip;
    u->close_state = close_state;
    u->out_close = out_close;
    n->user = u;
    n->measure_fn = measure_chip;
    n->draw_fn = draw_chip;
    n->hit_fn = hit_chip;
    n->debug_name = "chip";
    return n;
}

LayoutNode* CategoryList(LayoutArena& arena, const char* const* labels,
                         const char* const* probes, int count, int active,
                         ButtonState* states, bool* const* out_clicked) {
    LayoutNode* node = make_node(arena, NodeKind::Leaf);
    auto* user = arena.alloc<CategoryUser>();
    *user = {labels, probes, std::max(0, count), active, states, out_clicked};
    node->user = user;
    node->width = SizeSpec::fill();
    node->measure_fn = measure_categories;
    node->hit_fn = hit_categories;
    node->draw_fn = draw_categories;
    node->debug_name = "category_list";
    return node;
}

LayoutNode* Segmented(LayoutArena& arena, const char* const* labels,
                      const char* const* tooltips, int count, int active,
                      ButtonState* states, bool* const* out_clicked,
                      SizeSpec width) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<SegmentedUser>();
    u->labels = labels;
    u->tooltips = tooltips;
    u->count = count;
    u->active = active;
    u->states = states;
    u->out_clicked = out_clicked;
    n->user = u;
    n->width = width;
    n->measure_fn = measure_segmented;
    n->draw_fn = draw_segmented;
    n->hit_fn = hit_segmented;
    n->debug_name = "segmented";
    return n;
}

LayoutNode* IconButton(LayoutArena& arena, Icon icon, ButtonState* state,
                       bool* out_clicked, const ButtonOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<IconUser>();
    u->framed = opts.framed;
    u->icon = icon;
    u->state = state;
    u->out_clicked = out_clicked;
    u->disabled = opts.disabled;
    u->active = opts.active;
    u->tooltip = opts.tooltip;
    u->probe = opts.probe;
    n->user = u;
    n->width = opts.width;
    n->measure_fn = measure_icon;
    n->draw_fn = draw_icon_button;
    n->hit_fn = hit_enabled<IconUser>;
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
    if (req.kind == Context::PopupKind::Color) {
        run_color_popup(canvas, font, theme, ctx, req, input);
        return;
    }
    auto* st = static_cast<DropdownState*>(req.state);
    if (!st || !st->open) return;
    const bool owns = ctx.widget_owns_mouse(ctx.acquire_widget_id(st));

    const Rect& r = req.rect;
    draw_popup_chrome(canvas, theme, r);
    for (int i = 0; i < req.count; ++i) {
        const Rect ir{r.x + 4.0f,
                      r.y + 4.0f + static_cast<float>(i) * kPopupRowH,
                      r.w - 8.0f, kPopupRowH};
        probe_add(std::string("opt:") + req.items[i], ir);
        const bool hover = ir.contains(input.mouse);
        draw_popup_row(canvas, theme, ir, hover);
        const Color fg = i == req.selected
            ? theme.accent
            : (hover ? theme.text : theme.text_dim);
        draw_text(canvas, font, req.items[i],
                  {ir.x + 6.0f,
                   ir.y + (ir.h - font.line_height() * theme.font_size) *
                              0.5f},
                  theme.font_size, fg);
        if (owns && hover && input.left_pressed()) {
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
    n->hit_fn = hit_state<ScrubberUser>;
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
    n->hit_fn = hit_state<CheckboxUser>;
    n->debug_name = "checkbox";
    return n;
}

static LayoutNode* slider_node(LayoutArena& arena, float* value,
                               float min_value, float max_value,
                               SliderState* state, const SliderOpts& opts,
                               void (*draw)(LayoutNode&, LayoutFrame&),
                               const char* debug_name) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<SliderUser>();
    u->value = value;
    u->min_value = min_value;
    u->max_value = max_value;
    u->state = state;
    u->opts = opts;
    n->user = u;
    n->width = SizeSpec::fill();
    n->measure_fn = measure_slider;
    n->draw_fn = draw;
    n->hit_fn = hit_slider;
    n->debug_name = debug_name;
    return n;
}

LayoutNode* SliderF(LayoutArena& arena, float* value, float min_value,
                    float max_value, SliderState* state,
                    const SliderOpts& opts) {
    return slider_node(arena, value, min_value, max_value, state, opts,
                       draw_slider, "slider");
}

LayoutNode* DialF(LayoutArena& arena, float* value, float min_value,
                  float max_value, SliderState* state,
                  const SliderOpts& opts) {
    return slider_node(arena, value, min_value, max_value, state, opts,
                       draw_dial, "dial");
}

LayoutNode* ColorSwatch(LayoutArena& arena, const float rgba[4],
                        SwatchState* state, float* out_rgba,
                        bool* out_changed, bool* out_released,
                        const SwatchOpts& opts) {
    LayoutNode* n = make_node(arena, NodeKind::Leaf);
    auto* u = arena.alloc<SwatchUser>();
    for (int i = 0; i < 4; ++i) u->rgba[i] = rgba[i];
    state->mode = opts.mode;
    state->hue_light = opts.hue_light;
    u->state = state;
    u->out_rgba = out_rgba;
    u->out_changed = out_changed;
    u->out_released = out_released;
    n->user = u;
    n->width = SizeSpec::fill();
    n->height = SizeSpec::fixed(14.0f);
    n->measure_fn = measure_swatch;
    n->draw_fn = draw_swatch;
    n->hit_fn = hit_swatch;
    n->debug_name = "swatch";
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
    n->hit_fn = hit_state<SectionUser>;
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
    u->bg = opts.bg;
    u->probe = opts.probe;
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
