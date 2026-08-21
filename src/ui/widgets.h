// Starter widget set. Reference idiom: factories allocate a per-widget blob
// in the frame arena, dupe label strings, and return a LayoutNode with
// static measure/draw/hit callbacks bound. Interaction state is
// CALLER-OWNED and persists across frames — widget identity is keyed on the
// state pointer (Context::acquire_widget_id).
//
// Output flags (out_clicked / out_changed / ...) are written during the
// draw pass of run_frame; read them after run_frame returns, then rebuild
// the tree next frame.

#pragma once

#include <string_view>

#include "ui/layout.h"
#include "ui/theme.h"

namespace looks::ui {

// ---- design tokens: one spacing rhythm for every panel. 4 inside a
// group, 8 between controls, 12 between groups; micro controls are 16 px.
inline constexpr float kSpaceTight = 4.0f;
inline constexpr float kSpaceUnit = 8.0f;
inline constexpr float kSpaceGroup = 12.0f;
inline constexpr float kMicroSize = 16.0f;

struct ButtonState {
    bool pressed = false;
    float hover_t = 0.0f;
    float press_t = 0.0f;
    float hover_seconds = 0.0f;   // sustained-hover clock for tooltips
};

struct SliderState {
    bool dragging = false;
    float hover_seconds = 0.0f;   // sustained-hover clock for tooltips
    // Shift fine-drag: relative from the anchor at 0.1x speed.
    bool fine = false;
    float fine_anchor_value = 0.0f;
    float fine_anchor_x = 0.0f;
    // DialF: last pointer angle, so drags apply relative rotation deltas.
    float dial_angle = 0.0f;
};

struct ScrubberState {
    bool dragging = false;
};

struct LabelOpts {
    float size = 0.0f;          // 0 = theme font_size
    Color color{0, 0, 0, 0};    // alpha 0 = theme text
    bool header = false;        // draw with the frame's header (serif) font
};

// Registers the node's clipped rect as a hit target keyed on `state` -
// the one hit/clip contract, shared with the app's custom widgets.
void register_rect_hit(LayoutNode& node, LayoutFrame& frame,
                       const void* state);

// Shared list-popup geometry: `count` rows sized to the widest label
// (at least `min_w`), opening below the anchor, flipped above when the
// viewport bottom would clip it and clamped inside horizontally. Every
// menu-shaped overlay derives its rect here so the off-screen policy
// cannot fork per menu.
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

// Clickable fold header: chevron + serif label, full-row hit target.
// `open` only affects the chevron — the caller owns the fold state and
// skips building the section body when folded. `small` renders a nested
// level: body font, indented (add-effect categories).
LayoutNode* SectionHeader(LayoutArena& arena, std::string_view text,
                          bool open, ButtonState* state, bool* out_clicked,
                          bool small = false);

struct ButtonOpts {
    bool disabled = false;
    bool flat = false;          // micro control: chrome only on hover
    bool align_left = false;    // list-item labels (layer/group names)
    // Lit state: icon draws in accent — the rail's key/route dots
    // show keyed/routed params exactly like the canvas rows.
    bool active = false;
    SizeSpec width;             // default: hug label (min 64; flat: hug+8)
    const char* tooltip = nullptr;   // shown after a hover delay
    // Right-click report: the app opens its context menu for this row.
    bool* out_ctx = nullptr;
};

LayoutNode* Button(LayoutArena& arena, std::string_view label,
                   ButtonState* state, bool* out_clicked,
                   const ButtonOpts& opts = {});

// Toggle chip — view-state switches (loop, live, a/b). Flat: dim text when
// off, accent text on a quiet fill when on. Never looks like an action.
LayoutNode* Chip(LayoutArena& arena, std::string_view label, bool on,
                 ButtonState* state, bool* out_clicked,
                 const char* tooltip = nullptr);

// Icons drawn with anti-aliased primitives only (SDF rects, thin strokes,
// font glyphs) — filled triangles alias and are avoided except Play.
// Wave = modulation route, Key = keyframe diamond, Knob = macro dot,
// Eye/EyeOff = enabled/bypassed, Dice = randomize, Link = grouping,
// Solo/SoloOn = stack solo (accent when active), Copy = duplicate.
enum class Icon : uint8_t {
    Play, Pause, Up, Down, Close, Wave, Key, Knob, Eye, EyeOff, Dice, Link,
    Solo, SoloOn, Copy,
};

LayoutNode* IconButton(LayoutArena& arena, Icon icon, ButtonState* state,
                       bool* out_clicked, const ButtonOpts& opts = {});

// Timeline scrubber: thin track + playhead line. Reads as transport, not
// as a parameter slider.
LayoutNode* Scrubber(LayoutArena& arena, float* frame, float frame_count,
                     ScrubberState* state, bool* out_changed);

struct DropdownState {
    bool open = false;
    ButtonState button;
};

// Enum/value picker — the ONLY pattern for multi-choice values (no
// click-to-cycle buttons). Closed: current value + caret, constant height.
// Open: an OVERLAY list drawn by RunPopup — opening never reflows the
// surrounding layout. A chosen index lands in *out_selected.
LayoutNode* Dropdown(LayoutArena& arena, const char* const* items, int count,
                     int selected, DropdownState* state, int* out_selected,
                     SizeSpec width = {}, const char* tooltip = nullptr);

// Draws + interacts with the open dropdown overlay. MUST run right after
// run_frame and BEFORE the frame's edit handlers, so a selection made this
// frame is applied this frame.
void RunPopup(Canvas2D& canvas, const Font& font, const Theme& theme,
              Context& ctx, UiInput& input);

LayoutNode* Checkbox(LayoutArena& arena, std::string_view label, bool* value,
                     ButtonState* state, bool* out_changed = nullptr);

struct SliderOpts {
    const char* format = "%.2f";   // value readout; nullptr hides it
    bool* out_changed = nullptr;   // value moved this frame
    bool* out_released = nullptr;  // drag ended this frame (undo coalescing)
    // v5.7 real controls: a press on the VALUE TEXT reports here instead
    // of starting a drag — the caller opens its inline type-in editor.
    bool* out_value_clicked = nullptr;
    // Readout multiplier (display only): degree readouts over radian
    // values. Type-in callers must divide by it on commit.
    float display_scale = 1.0f;
    // Readout offset, added after the multiplier (display only):
    // corner-origin pixel readouts over centered-fraction values.
    // Type-in callers must subtract it before the divide.
    float display_offset = 0.0f;
    const char* tooltip = nullptr;   // sustained-hover tip (range/default)
    // Right-click report: the app opens its context menu for this row.
    bool* out_ctx = nullptr;
};

LayoutNode* SliderF(LayoutArena& arena, float* value, float min_value,
                    float max_value, SliderState* state,
                    const SliderOpts& opts = {});

// Rotary control for angle params: knob + pointer on the left, the
// slider's value readout (and type-in hotspot) on the right. Dragging
// anywhere in the row rotates RELATIVE to the press — the pointer never
// jumps to the cursor, and multi-turn ranges accumulate through wraps.
// The pointer shows the DISPLAY angle (value * display_scale) modulo one
// turn, 0 at 12 o'clock, clockwise positive; shift = 0.1x fine rotation.
LayoutNode* DialF(LayoutArena& arena, float* value, float min_value,
                  float max_value, SliderState* state,
                  const SliderOpts& opts = {});

struct SwatchState {
    // `open` first: widget identity keys on state ADDRESSES, and the chip
    // (&state.button) must not alias the popup layer (&state).
    bool open = false;
    ButtonState button;
    // Editing truth while the picker is open — kept in HSV so hue survives
    // grayscale rgb (rgb->hsv is lossy at sat 0).
    float hue = 0.0f;
    float sat = 0.0f;
    float val = 1.0f;
    int drag_zone = 0;   // 1 = sv square, 2 = hue strip
};

// Color chip that opens a picker overlay on click (RunPopup, same
// single-open-popup contract as Dropdown). While the picker drags, the
// chosen color streams into out_rgb[3] with out_changed raised;
// out_released marks drag end for undo coalescing.
LayoutNode* ColorSwatch(LayoutArena& arena, const float rgb[3],
                        SwatchState* state, float* out_rgb,
                        bool* out_changed, bool* out_released);

struct PanelOpts {
    Edges padding = Edges::all(12);
    float corner_radius = -1.0f;   // <0 = theme
    bool outline = true;           // hairline; nested cards go without
    bool accent_edge = false;      // 2 px accent bar on the left (selection)
    Color bg{0, 0, 0, 0};          // alpha 0 = theme panel_bg
};

// Background + outline drawn behind `child`.
LayoutNode* Panel(LayoutArena& arena, LayoutNode* child,
                  const PanelOpts& opts = {});

LayoutNode* Separator(LayoutArena& arena);

}  // namespace looks::ui
