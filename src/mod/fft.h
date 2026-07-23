// Hand-rolled radix-2 FFT (spec §12) for the import-time audio analysis.
// In-place, iterative, f32. Sizes must be powers of two.

#pragma once

#include <cstdint>
#include <vector>

namespace looks::mod {

struct Complex {
    float re = 0.0f;
    float im = 0.0f;
};

// In-place forward FFT; data.size() must be a power of two.
void fft(std::vector<Complex>& data);

// Magnitude spectrum of a real signal window (Hann applied). Returns
// n/2 + 1 magnitudes. `samples` shorter than n is zero-padded.
std::vector<float> magnitude_spectrum(const float* samples, size_t count,
                                      size_t n);

}  // namespace looks::mod
