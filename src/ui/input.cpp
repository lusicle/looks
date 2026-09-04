#include "ui/input.h"

namespace looks::ui {

namespace {

uint8_t button_bit(platform::MouseButton b) {
    switch (b) {
        case platform::MouseButton::Left: return kMouseLeft;
        case platform::MouseButton::Right: return kMouseRight;
        case platform::MouseButton::Middle: return kMouseMiddle;
        default: return 0;
    }
}

}  // namespace

void UiInput::begin_frame(const std::vector<platform::Event>& events,
                          float dpi_scale) {
    const Vec2 prev_raw = mouse_raw;
    buttons_pressed = 0;
    buttons_released = 0;
    wheel_y = 0.0f;
    keys.clear();

    const float inv = dpi_scale > 1e-3f ? 1.0f / dpi_scale : 1.0f;
    for (const platform::Event& e : events) {
        switch (e.type) {
            case platform::Event::Type::MouseMove:
                mouse_raw = {e.mouse_x * inv, e.mouse_y * inv};
                mods = e.mods;
                break;
            case platform::Event::Type::MouseDown: {
                mouse_raw = {e.mouse_x * inv, e.mouse_y * inv};
                const uint8_t bit = button_bit(e.button);
                buttons_down |= bit;
                buttons_pressed |= bit;
                mods = e.mods;
                break;
            }
            case platform::Event::Type::MouseUp: {
                mouse_raw = {e.mouse_x * inv, e.mouse_y * inv};
                const uint8_t bit = button_bit(e.button);
                buttons_down &= static_cast<uint8_t>(~bit);
                buttons_released |= bit;
                mods = e.mods;
                break;
            }
            case platform::Event::Type::MouseWheel:
                wheel_y += e.wheel_y;
                mods = e.mods;
                break;
            case platform::Event::Type::Char:
                break;
            case platform::Event::Type::KeyDown:
                mods = e.mods;
                break;
            case platform::Event::Type::KeyUp:
                mods = e.mods;
                break;
            case platform::Event::Type::FocusLost:
                buttons_down = 0;
                break;
            default:
                break;
        }
    }
    mouse = mouse_raw;
    mouse_delta = mouse_raw - prev_raw;
}

}  // namespace looks::ui
