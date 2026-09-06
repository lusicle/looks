// The caller owns the widget state and it must live across frames.
// run_frame writes the out flags during draw; read them after it returns.

#pragma once

#include <cmath>
#include <string_view>

#include "ui/interact.h"
#include "ui/layout.h"
#include "ui/theme.h"

namespace looks::ui {

inline constexpr float kSpaceTight = 4.0f;
inline constexpr float kSpaceUnit = 8.0f;
inline constexpr float kMicroSize = 16.0f;

// The format decimal count sets the drag step for sliders and dials.
inline float snap_to_format(float v, const char* format) {
    if (!format) return v;   // no readout, so there is no step
    int decimals = 2;
    for (const char* c = format; *c; ++c)
        if (*c == '.' && c[1] >= '0' && c[1] <= '9') {
            decimals = c[1] - '0';
            break;
        }
    float step = 1.0f;
    for (int i = 0; i < decimals; ++i) step *= 0.1f;
    return std::round(v / step) * step;
}

struct ButtonState {
    bool pressed = false;
    float hover_t = 0.0f;
    float press_t = 0.0f;
    float hover_seconds = 0.0f;   // sustained-hover clock for tooltips
};

float transition_step(float current, bool on, float dt);

enum class Icon : uint8_t {
    Play, Pause, Up, Down, Close, Wave, Key, Knob, Eye, EyeOff, Dice, Link,
    Solo, SoloOn, Copy, Lock, Magnet, ChevronRight,
};

struct SliderState {
    bool dragging = false;
    float hover_seconds = 0.0f;   // sustained-hover clock for tooltips
    // Shift fine-drag: relative from the anchor at 0.1x speed.
    bool fine = false;
    float fine_anchor_value = 0.0f;
    float fine_anchor_x = 0.0f;
    // DialF: last pointer angle in degrees; drags apply relative deltas.
    float dial_angle = 0.0f;
};

struct ScrubberState {
    bool dragging = false;
    // A press stays a click until the frame value moves.
    bool moved = false;
    float press_value = -1.0f;
};

struct LabelOpts {
    float size = 0.0f;          // 0 = theme font_size
    Color color{0, 0, 0, 0};    // alpha 0 = theme text
    bool header = false;        // draw with the frame's header (serif) font
};

void register_rect_hit(LayoutNode& node, LayoutFrame& frame,
                       const void* state);

// The rect flips above the anchor and clamps inside the viewport.
inline constexpr float kPopupRowH = 20.0f;
Rect list_popup_rect(const Rect& anchor, float min_w,
                     const char* const* items, int count, const Font& font,
                     float font_size, Vec2 viewport);
Rect list_popup_rect(const Rect& anchor, float min_w,
                     const char* const* items, int count,
                     const LayoutFrame& frame);

LayoutNode* Label(LayoutArena& arena, std::string_view text,
                  const LabelOpts& opts = {});
LayoutNode* Heading(LayoutArena& arena, std::string_view text);

// open only draws the chevron; the caller skips the body when folded.
LayoutNode* SectionHeader(LayoutArena& arena, std::string_view text,
                          bool open, ButtonState* state, bool* out_clicked,
                          bool small = false);

struct ButtonOpts {
    bool disabled = false;
    bool flat = false;          // micro control: chrome only on hover
    bool align_left = false;
    bool active = false;
    SizeSpec width;             // default: hug label (min 64; flat: hug+8)
    const char* tooltip = nullptr;   // shown after a hover delay
    // Right-click sets this flag.
    bool* out_ctx = nullptr;
    const char* probe = nullptr;
    bool* out_hovered = nullptr;
    int trailing_icon = -1;
};

LayoutNode* Button(LayoutArena& arena, std::string_view label,
                   ButtonState* state, bool* out_clicked,
                   const ButtonOpts& opts = {});

// Pass close_state and out_close together or not at all.
LayoutNode* Chip(LayoutArena& arena, std::string_view label, bool on,
                 ButtonState* state, bool* out_clicked,
                 const char* tooltip = nullptr,
                 ButtonState* close_state = nullptr,
                 bool* out_close = nullptr);

// states and out_clicked are arrays of count entries.
LayoutNode* Segmented(LayoutArena& arena, const char* const* labels,
                      const char* const* tooltips, int count, int active,
                      ButtonState* states, bool* const* out_clicked,
                      SizeSpec width = {});

LayoutNode* IconButton(LayoutArena& arena, Icon icon, ButtonState* state,
                       bool* out_clicked, const ButtonOpts& opts = {});

// Draws the icon only: no hover, no hit test, no probe.
void draw_icon_glyph(Canvas2D& canvas, const Font& font, Icon icon,
                     Vec2 center, Color color, float font_size);

LayoutNode* Scrubber(LayoutArena& arena, float* frame, float frame_count,
                     ScrubberState* state, bool* out_changed);

struct DropdownState {
    bool open = false;
    ButtonState button;
};

// RunPopup draws the open list as an overlay; the layout never reflows.
LayoutNode* Dropdown(LayoutArena& arena, const char* const* items, int count,
                     int selected, DropdownState* state, int* out_selected,
                     SizeSpec width = {}, const char* tooltip = nullptr);

// Call after run_frame and before the frame edit handlers.
void RunPopup(Canvas2D& canvas, const Font& font, const Theme& theme,
              Context& ctx, UiInput& input);

LayoutNode* Checkbox(LayoutArena& arena, std::string_view label, bool* value,
                     ButtonState* state, bool* out_changed = nullptr);

struct TextInputState {
    ButtonState button;
    bool had_focus = false;
};

struct TextNav {
    bool up = false;
    bool down = false;
    bool tab = false;
};

TextResult text_input_keys(TextField& field, UiInput& input,
                           TextNav* nav = nullptr);

struct TextInputOpts {
    const char* placeholder = nullptr;
    const char* prefix = nullptr;
    const char* text = nullptr;
    const char* probe = nullptr;
    const char* tooltip = nullptr;
    bool flat = false;
    bool small = false;
    bool grab_focus = false;
    SizeSpec width = SizeSpec::fill();
    bool* out_clicked = nullptr;
    bool* out_changed = nullptr;
    bool* out_commit = nullptr;
    bool* out_cancel = nullptr;
    bool* out_blur = nullptr;
    TextNav* nav = nullptr;
};

LayoutNode* TextInput(LayoutArena& arena, TextField* field,
                      TextInputState* state, const TextInputOpts& opts = {});
bool text_input_focused(const Context& ctx, const TextInputState* state);
void text_input_focus(Context& ctx, const TextInputState* state);

struct TextCaret {
    int caret = -1;
    int sel_lo = 0;
    int sel_hi = 0;
};
bool caret_blink_on(const Context& ctx);
TextCaret field_caret(const TextField& f, int offset, bool blink_on);

struct TextHostOuts {
    bool* clicked = nullptr;
    bool* changed = nullptr;
    bool* commit = nullptr;
    bool* cancel = nullptr;
    bool* blur = nullptr;
    TextNav* nav = nullptr;
};
struct TextHostResult {
    bool focused = false;
    bool ended = false;
};
TextHostResult text_host_frame(LayoutFrame& frame, WidgetId id,
                               TextInputState& st, TextField& field,
                               const Rect& r, bool grab_focus,
                               const TextHostOuts& outs);

struct SliderOpts {
    const char* format = "%.2f";   // value readout; nullptr hides it
    bool* out_changed = nullptr;   // value moved this frame
    bool* out_released = nullptr;  // drag ended this frame (undo coalescing)
    // A press on the value text sets this instead of starting a drag.
    bool* out_value_clicked = nullptr;
    // Display multiplier; type-in callers must divide by it on commit.
    float display_scale = 1.0f;
    // Added after the multiplier; type-in subtracts it before it divides.
    float display_offset = 0.0f;
    const char* tooltip = nullptr;   // sustained-hover tip (range/default)
    // Right-click sets this flag.
    bool* out_ctx = nullptr;
    TextField* edit = nullptr;
    TextInputState* edit_state = nullptr;
    bool* out_commit = nullptr;
    bool* out_cancel = nullptr;
    bool* out_blur = nullptr;
};

LayoutNode* SliderF(LayoutArena& arena, float* value, float min_value,
                    float max_value, SliderState* state,
                    const SliderOpts& opts = {});

// A drag rotates relative to the press; the pointer never jumps.
// The pointer shows the display angle: 0 at 12 o'clock, clockwise.
LayoutNode* DialF(LayoutArena& arena, float* value, float min_value,
                  float max_value, SliderState* state,
                  const SliderOpts& opts = {});

enum class SwatchMode : uint8_t { Rgb, Rgba, Hue };

struct SwatchState {
    // Keep open first: &state must not alias &state.button.
    bool open = false;
    ButtonState button;
    SwatchMode mode = SwatchMode::Rgb;
    float hue_light = 0.5f;
    // HSV keeps the hue that a gray rgb value cannot store.
    float hue = 0.0f;
    float sat = 0.0f;
    float val = 1.0f;
    float alpha = 1.0f;
    int drag_zone = 0;   // 1 = sv square, 2 = hue strip, 3 = alpha strip
    // 0-2 = the R/G/B fields, 3 = alpha, 4 = hex, -1 = none focused.
    int field_edit = -1;
    TextField edit;
};

void hsv_to_rgb(float h, float s, float v, float out[3]);
void rgb_to_hsv(const float rgb[3], float& h, float& s, float& v);
Color hue_bar_color(float hue01, float lightness);
Rect swatch_popup_rect(const Rect& anchor, const SwatchState& st,
                       const LayoutFrame& frame);
// Commits the focused entry field; it does nothing when none has focus.
void swatch_commit_field(SwatchState& st, float* out_rgba, bool* out_changed,
                         bool* out_released);
void swatch_seed_from(SwatchState& st, const float rgba[4]);
void draw_swatch_face(Canvas2D& canvas, const Rect& r, float radius,
                      SwatchMode mode, const float rgba[4],
                      float hue_light = 0.5f);

struct SwatchOpts {
    SwatchMode mode = SwatchMode::Rgb;
    float hue_light = 0.5f;
};

// The picker streams out_rgba while it drags; out_released marks the end.
LayoutNode* ColorSwatch(LayoutArena& arena, const float rgba[4],
                        SwatchState* state, float* out_rgba,
                        bool* out_changed, bool* out_released,
                        const SwatchOpts& opts = {});

struct PanelOpts {
    Edges padding = Edges::all(12);
    float corner_radius = -1.0f;   // <0 = theme
    bool outline = true;
    bool accent_edge = false;      // 2 px accent bar on the left (selection)
    Color bg{0, 0, 0, 0};          // alpha 0 = theme panel_bg
};

LayoutNode* Panel(LayoutArena& arena, LayoutNode* child,
                  const PanelOpts& opts = {});

LayoutNode* Separator(LayoutArena& arena);

struct ButtonFace {
    float hover_t = 0.0f;
    float press_t = 0.0f;
    bool active = false;
    bool disabled = false;
    bool flat = false;
    bool align_left = false;
    float font_size = 13.0f;
    float radius = 3.0f;
    float scale = 1.0f;
    int trailing_icon = -1;
};
void draw_button_face(Canvas2D& canvas, const Font& font, const Theme& theme,
                      const Rect& r, std::string_view label,
                      const ButtonFace& face, const Rect& clip = {});
float value_box_width(const Font& font, std::string_view value,
                      float font_size, float scale);
Rect slider_value_rect(const Rect& r, float box_w);
Rect slider_track_rect(const Rect& r, float box_w, float scale);
void draw_slider_track(Canvas2D& canvas, const Theme& theme, const Rect& track,
                       float t, float live_t, bool active, float scale);
void draw_dial_face(Canvas2D& canvas, const Theme& theme, Vec2 center,
                    float knob_radius, float deg, bool active, float live_deg,
                    bool has_live, float scale);
void draw_dropdown_face(Canvas2D& canvas, const Font& font, const Theme& theme,
                        const Rect& r, std::string_view text, bool open,
                        float hover_t, float font_size, float radius,
                        float scale, const Rect& clip = {});
void draw_text_input_face(Canvas2D& canvas, const Font& font,
                          const Theme& theme, const Rect& r,
                          std::string_view text, const TextCaret& caret,
                          bool focused, bool flat, bool dim, float hover_t,
                          float font_size, float radius, float scale,
                          const Rect& clip = {});
void draw_popup_chrome(Canvas2D& canvas, const Theme& theme, const Rect& r);
void draw_popup_row(Canvas2D& canvas, const Theme& theme, const Rect& r,
                    bool hover);

}  // namespace looks::ui
