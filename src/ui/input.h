#pragma once

#include <cstdint>
#include <vector>

#include "platform/window.h"
#include "ui/types.h"

namespace looks::ui {

struct UiInput {
    Vec2 mouse{};          // logical px; modals may deaden it per frame
    Vec2 mouse_delta{};
    // Only begin_frame writes this; mouse rebuilds from it each frame.
    Vec2 mouse_raw{};
    uint8_t buttons_down = 0;
    uint8_t buttons_pressed = 0;    // edges this frame
    uint8_t buttons_released = 0;
    float wheel_y = 0.0f;           // scroll notches; the Context owns it
    uint32_t mods = 0;              // platform::kModCtrl / Shift / Alt
    // What the key chain did not take, for the focused widget to read.
    std::vector<platform::Event> keys;

    bool left_down() const { return buttons_down & kMouseLeft; }
    bool left_pressed() const { return buttons_pressed & kMouseLeft; }
    bool left_released() const { return buttons_released & kMouseLeft; }
    bool right_pressed() const { return buttons_pressed & kMouseRight; }

    // Divides physical event coordinates by dpi_scale to get logical px.
    void begin_frame(const std::vector<platform::Event>& events, float dpi_scale);
};

}  // namespace looks::ui
