// Counter-based deterministic hashing: all randomness derives
// from seeded hashes keyed on (seed, frame, cell...) — no stateful RNG
// anywhere. Same inputs => same values on every platform.

#pragma once

#include <cstddef>
#include <cstdint>

namespace looks {

// splitmix64 finalizer — good avalanche, trivially portable.
inline uint64_t hash_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline uint64_t hash_combine(uint64_t seed, uint64_t value) {
    return hash_u64(seed ^ (value + 0x9E3779B97F4A7C15ull));
}

// Uniform [0, 1).
inline float hash_to_float01(uint64_t h) {
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

inline float hash_float01(uint64_t seed, uint64_t counter) {
    return hash_to_float01(hash_combine(seed, counter));
}

// FNV-1a over bytes: stable content fingerprints (cache contexts, the
// determinism harness, bundle directory names). Separate from the
// seeded counter family above on purpose - a fingerprint must not
// change when the effect hash does.
inline constexpr uint64_t kFnvOffset = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t fnv1a(const void* data, size_t size,
                      uint64_t seed = kFnvOffset) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * kFnvPrime;
    return hash;
}

}  // namespace looks
