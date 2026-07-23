// Small 2D math shared by UI and (later) the render graph's 2D passes.

#pragma once

#include <cmath>

namespace looks {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 r) const { return {x + r.x, y + r.y}; }
    constexpr Vec2 operator-(Vec2 r) const { return {x - r.x, y - r.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(Vec2 r) { x += r.x; y += r.y; return *this; }
    constexpr Vec2& operator-=(Vec2 r) { x -= r.x; y -= r.y; return *this; }
    constexpr bool operator==(const Vec2&) const = default;

    float length() const { return std::sqrt(x * x + y * y); }
    constexpr float length_sq() const { return x * x + y * y; }
    Vec2 normalized_or_zero() const {
        float len = length();
        return len > 1e-8f ? Vec2{x / len, y / len} : Vec2{};
    }
    // 90° counter-clockwise perpendicular (screen space, Y-down).
    constexpr Vec2 perp_ccw() const { return {y, -x}; }
};

constexpr float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

}  // namespace looks
