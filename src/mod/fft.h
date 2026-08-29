#pragma once

#include <cstdint>
#include <vector>

namespace looks::mod {

struct Complex {
    float re = 0.0f;
    float im = 0.0f;
};

// The size of data must be a power of two.
void fft(std::vector<Complex>& data);

// Returns n/2 + 1 magnitudes. A short input is zero-padded.
std::vector<float> magnitude_spectrum(const float* samples, size_t count,
                                      size_t n);

}  // namespace looks::mod
