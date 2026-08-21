// UiInput — per-frame input snapshot in LOGICAL px, decoupled from the
// platform event queue. Widgets read edges/holds from here and set
// `consumed` when they claim the pointer (the hit-test winner does this
// automatically via Context).

#pragma once

#include <cstdint>
#include <vector>

#include "platform/window.h"
#include "ui/types.h"

namespace looks::ui {

inline constexpr uint8_t kMouseLeft = 1u << 0;
inline constexpr uint8_t kMouseRight = 1u << 1;
inline constexpr uint8_t kMouseMiddle = 1u << 2;

struct UiInput {
    Vec2 mouse{};          // logical px
    Vec2 mouse_delta{};
    uint8_t buttons_down = 0;
    uint8_t buttons_pressed = 0;    // edges this frame
    uint8_t buttons_released = 0;
    float wheel_y = 0.0f;           // scroll notches; consumer zeroes it
    float wheel_x = 0.0f;
    uint32_t mods = 0;              // platform::kModCtrl / Shift / Alt
    std::vector<uint32_t> typed;    // UTF-32 chars this frame
    bool consumed = false;          // pointer claimed by UI this frame

    bool left_down() const { return buttons_down & kMouseLeft; }
    bool left_pressed() const { return buttons_pressed & kMouseLeft; }
    bool left_released() const { return buttons_released & kMouseLeft; }
    bool right_pressed() const { return buttons_pressed & kMouseRight; }

    // Folds the platform events into the snapshot. `to_logical` divides
    // physical event coordinates by the window's DPI scale.
    void begin_frame(const std::vector<platform::Event>& events, float dpi_scale);
};

}  // namespace looks::ui
