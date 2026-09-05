// Keep this file Vulkan-free; tests link it without a GPU.
// Input halves are tight-packed RGBA16F.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace looks::doc {
struct EffectInstance;
}

namespace looks::gfx {

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

// The engine keys one EdState per effect id.
struct EdState {
    // Level indices, 3x5 bits per pixel; GPU expand decodes via ed_level_table.
    std::vector<uint32_t> out;
    std::vector<uint8_t> idx;     // per-channel pick planes
    std::vector<float> carry;     // quantization error, planar RGB
    std::vector<float> work;      // working buffer, planar RGB
    std::vector<uint32_t> hilbert;   // Riemersma visit order
    uint32_t hilbert_w = 0, hilbert_h = 0;
};

void run_error_diffusion(const uint16_t* halves, uint32_t width,
                         uint32_t height, const doc::EffectInstance& fx,
                         EdState& slot);

// Linear-light palette; GPU expand must match bit for bit, never re-derive.
int ed_level_table(const doc::EffectInstance& fx, float out_levels[17]);

}  // namespace looks::gfx
