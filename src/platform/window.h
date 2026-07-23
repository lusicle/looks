// Platform window + input abstraction (spec §1: all platform code stays in
// src/platform/ so a macOS port swaps implementations, not callers).
//
// Model: one window, polled events. Each frame the app drains the queue via
// pump_events(); nothing here blocks. Sizes and mouse positions are in
// PHYSICAL pixels — the UI applies its own scale (mirrors the reference
// toolkit, which feeds physical px into the draw list and lets the
// projection handle the rest). dpi_scale() is the per-monitor factor
// (96 dpi == 1.0).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace looks::platform {

enum class Key : uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Tab, CapsLock, Shift, Ctrl, Alt, Space, Enter, Backspace,
    Insert, Delete, Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    Minus, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon, Apostrophe, Comma, Period, Slash, Grave,
    Count
};

enum class MouseButton : uint8_t { Left, Right, Middle, X1, X2 };

// Modifier bitmask carried on key and mouse events.
inline constexpr uint32_t kModCtrl = 1u << 0;
inline constexpr uint32_t kModShift = 1u << 1;
inline constexpr uint32_t kModAlt = 1u << 2;

struct Event {
    enum class Type {
        CloseRequested,
        Resize,        // width/height: new client size, physical px
        DpiChanged,    // dpi_scale
        FocusGained,
        FocusLost,
        MouseMove,     // mouse_x/mouse_y
        MouseDown,     // button + mouse position + mods
        MouseUp,
        MouseWheel,    // wheel_y in scroll lines (+ = away from user)
        KeyDown,       // key, repeat, mods
        KeyUp,
        Char,          // codepoint (UTF-32), for text input
        FileDrop,      // drop_path: first file of a drag-and-drop
    };

    Type type;
    std::string drop_path;   // UTF-8; FileDrop only
    int32_t width = 0;
    int32_t height = 0;
    float dpi_scale = 1.0f;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    MouseButton button = MouseButton::Left;
    float wheel_x = 0.0f;
    float wheel_y = 0.0f;
    Key key = Key::Unknown;
    bool repeat = false;
    uint32_t mods = 0;
    uint32_t codepoint = 0;
};

struct WindowDesc {
    std::string title = "looks";   // UTF-8
    int width = 1600;              // desired client size, logical px (scaled by DPI)
    int height = 900;
    bool resizable = true;
};

class Window {
public:
    virtual ~Window() = default;

    // Drains pending OS messages into `out`. Returns false once the window
    // has been destroyed (after the app reacts to CloseRequested).
    virtual bool pump_events(std::vector<Event>& out) = 0;

    virtual uint32_t width() const = 0;    // client area, physical px
    virtual uint32_t height() const = 0;
    virtual float dpi_scale() const = 0;
    virtual bool minimized() const = 0;

    virtual void set_title(const std::string& utf8) = 0;
    virtual void request_close() = 0;      // destroys the native window

    // Native handles for the graphics backend (HWND / HINSTANCE on win32).
    virtual void* native_window() const = 0;
    virtual void* native_instance() const = 0;
};

// Must be called once before create_window (sets process DPI awareness).
void init();

std::unique_ptr<Window> create_window(const WindowDesc& desc);

}  // namespace looks::platform
