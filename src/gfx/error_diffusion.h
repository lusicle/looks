// CPU error diffusion (the ErrorDiffusion Codec-Box effect): pure buffer
// in/out with no GPU types, split from the engine so the exact-equivalence
// tests can link it without a Vulkan device. Input is tight-packed RGBA16F
// halves; output lands in EdState::out in the same format.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace looks::doc {
struct EffectInstance;
}

namespace looks::gfx {

// Half-float decode shared with the engine (flow-field readback).
inline float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) {
                man <<= 1;
                --exp;
            }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float out;
    static_assert(sizeof(out) == sizeof(bits));
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Per-instance persistent state (temporal carry, cached Hilbert walk,
// reused scratch). The engine keys one per effect id.
struct EdState {
    // Level indices packed 3x5 bits per pixel — the walk's real output is
    // picks, not colors, and shipping picks cuts the upload 4x. The GPU
    // expand pass reconstructs the linear palette from ed_level_table.
    std::vector<uint32_t> out;
    std::vector<uint8_t> idx;     // per-channel pick planes (walk scratch)
    std::vector<float> carry;     // quantization error, planar RGB
    std::vector<float> work;      // working buffer, planar RGB (reused)
    std::vector<uint32_t> hilbert;   // Riemersma visit order (cached)
    uint32_t hilbert_w = 0, hilbert_h = 0;
    uint32_t last_frame = 0xFFFFFFFFu;
    bool valid = false;
};

void run_error_diffusion(const uint16_t* halves, uint32_t width,
                         uint32_t height, const doc::EffectInstance& fx,
                         EdState& slot);

// The output palette in linear light, exactly as the walk's pick tables
// build it. The GPU expand pass must reproduce these values bit for bit,
// so they are computed once here and pushed — never re-derived in-shader.
int ed_level_table(const doc::EffectInstance& fx, float out_levels[17]);

}  // namespace looks::gfx
