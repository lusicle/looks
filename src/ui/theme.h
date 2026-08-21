// Theme — the concrete palette + metrics (dark surface ladder in the
// reference toolkit's idiom; colors authored as sRGB hex, stored linear).

#pragma once

#include "ui/types.h"

namespace looks::ui {

class Theme {
public:
    Color window_bg = Color::hex(0x101114);
    Color panel_bg = Color::hex(0x17181B);
    Color control_bg = Color::hex(0x1E2024);
    Color control_bg_hover = Color::hex(0x26282D);
    Color control_bg_active = Color::hex(0x141518);
    Color accent = Color::hex(0x4C9FE8);
    Color accent_dim = Color::hex(0x2E6390);
    Color hairline = Color::hex(0x2A2C31);
    Color text = Color::hex(0xE8EAED);
    Color text_dim = Color::hex(0x9AA0A6);
    Color text_disabled = Color::hex(0x5A5D63);

    float corner_radius = 3.0f;
    float stroke_width = 1.0f;
    float font_size = 13.0f;
    float font_size_small = 11.0f;
    float font_size_heading = 16.0f;
    float control_height = 22.0f;
};

const Theme& default_theme();

// Built-in palettes: graphite (default), night, ember, paper. The active
// theme is app-level state — widgets read it through LayoutFrame::theme
// every frame, so switching takes effect immediately. Persisted by the app
// (ui.json), not the project.
int theme_count();
const char* theme_name(int index);
const Theme& theme_preset(int index);
const Theme& active_theme();
void set_active_theme(int index);   // wrapped into range

inline Color lerp(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

}  // namespace looks::ui
