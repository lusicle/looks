// Synthetic dev-fixture signal, spelled once: the fixture generator,
// the determinism harness and the effect benches all sample these, so
// benches measure the same content the harness verifies. Moving
// diagonal gradient with a sweeping bright bar (limited-range luma) and
// drifting chroma ramps.
#pragma once

#include <cstdint>

namespace looks::devsynth {

inline uint8_t luma(uint32_t c, uint32_t r, uint32_t frame, uint32_t w) {
    uint32_t v = (c + r + frame * 3) & 0xFF;
    const uint32_t bar = (frame * 5) % w;
    if (c >= bar && c < bar + 24) v = 235;
    return static_cast<uint8_t>(16 + v * 219 / 255);
}

// Chroma sampled on the half-res grid (cx, cy).
inline uint8_t cb(uint32_t cx, uint32_t frame) {
    return static_cast<uint8_t>(96 + ((cx * 2 + frame) & 63));
}

inline uint8_t cr(uint32_t cy, uint32_t frame) {
    return static_cast<uint8_t>(160 - ((cy + frame) & 63));
}

}  // namespace looks::devsynth
