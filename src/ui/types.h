// UI core types (mirrors the reference toolkit's types.h — first-party
// implementation). Colors are LINEAR floats internally; to_rgba8() re-encodes
// RGB to sRGB bytes (keeps precision in the dark UI ladder) with linear
// alpha, and the vertex shaders decode back to linear so interpolation and
// blending stay linear. Units are logical px unless suffixed _physical.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "util/math2d.h"

namespace looks::ui {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr float left() const { return x; }
    constexpr float top() const { return y; }
    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }
    constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }

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

    // Perceptual (sRGB-encoded) inputs -> linear storage.
    static Color srgb(float r, float g, float b, float a = 1.0f) {
        return {srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), a};
    }
    static Color hex(uint32_t rgb, float a = 1.0f) {
        return srgb(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(rgb & 0xFF) / 255.0f, a);
    }

    Color with_alpha(float alpha) const { return {r, g, b, alpha}; }

    float luminance() const {  // Rec.709, linear
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    // Little-endian packed R,G,B,A bytes; RGB re-encoded to sRGB.
    uint32_t to_rgba8() const {
        auto encode = [](float linear) -> uint32_t {
            float e = linear_to_srgb(std::clamp(linear, 0.0f, 1.0f));
            return static_cast<uint32_t>(e * 255.0f + 0.5f);
        };
        uint32_t ab = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        return encode(r) | (encode(g) << 8) | (encode(b) << 16) | (ab << 24);
    }

    static float srgb_to_linear(float c) {
        return c <= 0.04045f ? c / 12.92f
                             : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    static float linear_to_srgb(float c) {
        return c <= 0.0031308f ? c * 12.92f
                               : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
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
