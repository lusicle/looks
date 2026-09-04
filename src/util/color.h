// These constants must stay equal to the shader prelude constants.
#pragma once

#include <cmath>
#include <cstdint>

namespace looks::color {

// Rec.709 luma weights for the encoded domain, not for linear light.
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

// Cb carries (B-Y)/kCb709. Cr carries (R-Y)/kCr709.
inline constexpr float kCb709 = 1.8556f;
inline constexpr float kCr709 = 1.5748f;
inline constexpr float kCb709G = kCb709 * kLumaB / kLumaG;
inline constexpr float kCr709G = kCr709 * kLumaR / kLumaG;

inline float luma709(float r, float g, float b) {
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

// Byte luma in 8.8 fixed point. 54/183/19 are the weights times 256.
inline uint8_t luma709_u8(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint8_t>((54u * r + 183u * g + 19u * b) >> 8);
}

// Limited-range BT.709 bytes to full-range RGB, 8.8 fixed point.
// The caller must clamp the results to 0..255.
inline void ycbcr709_to_rgb8(int y, int cb, int cr,
                             int* r, int* g, int* b) {
    const int yf = 298 * (y - 16) + 128;
    *r = (yf + 459 * (cr - 128)) >> 8;
    *g = (yf - 55 * (cb - 128) - 136 * (cr - 128)) >> 8;
    *b = (yf + 541 * (cb - 128)) >> 8;
}

// Full-range RGB bytes to limited-range BT.709, 8.8 fixed point.
// CPU-built I420 must use these constants or the shader decode shifts.
inline void rgb8_to_ycbcr709(int r, int g, int b, uint8_t* y, uint8_t* cb,
                             uint8_t* cr) {
    const int yl = 16 + ((47 * r + 157 * g + 16 * b + 128) >> 8);
    const int yf = (54 * r + 183 * g + 19 * b) >> 8;
    const int cbv = 128 + ((121 * (b - yf)) >> 8);
    const int crv = 128 + ((143 * (r - yf)) >> 8);
    auto clamp8 = [](int v, int lo, int hi) {
        return static_cast<uint8_t>(v < lo ? lo : (v > hi ? hi : v));
    };
    *y = clamp8(yl, 16, 235);
    *cb = clamp8(cbv, 16, 240);
    *cr = clamp8(crv, 16, 240);
}

// Inputs are byte values in limited range. Outputs are 0..1.
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

inline float hsl_channel(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

inline void hsl_to_rgb(float h, float s, float l, float* out) {
    if (s < 1e-6f) {
        out[0] = out[1] = out[2] = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    out[0] = hsl_channel(p, q, h + 1.0f / 3.0f);
    out[1] = hsl_channel(p, q, h);
    out[2] = hsl_channel(p, q, h - 1.0f / 3.0f);
}

}  // namespace looks::color
