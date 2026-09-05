// Colors are linear floats; to_rgba8 packs sRGB bytes, shaders decode back.
// Units are logical px unless the name says _physical.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "util/color.h"
#include "util/math2d.h"

namespace looks::ui {

inline constexpr uint8_t kMouseLeft = 1u << 0;
inline constexpr uint8_t kMouseRight = 1u << 1;
inline constexpr uint8_t kMouseMiddle = 1u << 2;

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }
    constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }

    constexpr Rect intersect(const Rect& o) const {
        float l = std::max(x, o.x);
        float t = std::max(y, o.y);
        float r = std::min(right(), o.right());
        float b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    }
    constexpr Rect inset(float amount) const {
        return {x + amount, y + amount, w - 2 * amount, h - 2 * amount};
    }
    constexpr bool operator==(const Rect&) const = default;
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static constexpr Color rgba(float r, float g, float b, float a = 1.0f) {
        return {r, g, b, a};
    }

    static Color srgb(float r, float g, float b, float a = 1.0f) {
        return {color::srgb_eotf(r), color::srgb_eotf(g), color::srgb_eotf(b), a};
    }
    static Color hex(uint32_t rgb, float a = 1.0f) {
        return srgb(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(rgb & 0xFF) / 255.0f, a);
    }

    Color with_alpha(float alpha) const { return {r, g, b, alpha}; }

    uint32_t to_rgba8() const {
        auto encode = [](float linear) -> uint32_t {
            float e = color::srgb_oetf(std::clamp(linear, 0.0f, 1.0f));
            return static_cast<uint32_t>(e * 255.0f + 0.5f);
        };
        uint32_t ab = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        return encode(r) | (encode(g) << 8) | (encode(b) << 16) | (ab << 24);
    }

    constexpr bool operator==(const Color&) const = default;
};

struct Edges {
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;

    static constexpr Edges all(float v) { return {v, v, v, v}; }
    static constexpr Edges xy(float x, float y) { return {x, y, x, y}; }
};

}  // namespace looks::ui
