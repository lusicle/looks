// Rec.709 / sRGB constants shared by every CPU conversion. The shader
// prelude (shaders/fx_common.slang) carries the same anchors; the two
// sides must agree or thumbnails, the value graph and the scopes
// disagree with the render.
#pragma once

#include <cmath>
#include <cstdint>

namespace looks::color {

// Encoded-domain Rec.709 luma weights.
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

// Chroma difference scales: Cb carries (B-Y)/kCb709, Cr carries
// (R-Y)/kCr709; the G-channel decode weights derive from the anchors.
inline constexpr float kCb709 = 1.8556f;
inline constexpr float kCr709 = 1.5748f;
inline constexpr float kCb709G = kCb709 * kLumaB / kLumaG;
inline constexpr float kCr709G = kCr709 * kLumaR / kLumaG;

inline float luma709(float r, float g, float b) {
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

// Byte luma in 8.8 fixed point (54/183/19 = the float weights * 256).
inline uint8_t luma709_u8(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint8_t>((54u * r + 183u * g + 19u * b) >> 8);
}

// Limited-range BT.709 bytes to full-range RGB, 8.8 fixed point.
// Callers clamp the results to 0..255.
inline void ycbcr709_to_rgb8(int y, int cb, int cr,
                             int* r, int* g, int* b) {
    const int yf = 298 * (y - 16) + 128;
    *r = (yf + 459 * (cr - 128)) >> 8;
    *g = (yf - 55 * (cb - 128) - 136 * (cr - 128)) >> 8;
    *b = (yf + 541 * (cb - 128)) >> 8;
}

// Limited-range expansions in float, byte inputs to 0..1 outputs.
inline float y709_norm(float y8) { return (y8 - 16.0f) * (1.0f / 219.0f); }
inline float chroma709_norm(float c8) {
    return (c8 - 128.0f) * (1.0f / 224.0f);
}

inline float srgb_eotf(float e) {
    return e <= 0.04045f ? e / 12.92f
                         : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

inline float srgb_oetf(float lin) {
    if (lin < 0.0f) lin = 0.0f;
    return lin <= 0.0031308f ? lin * 12.92f
                             : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
}

}  // namespace looks::color
