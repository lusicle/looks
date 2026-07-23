#include "mod/fft.h"

#include <cmath>

namespace looks::mod {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void fft(std::vector<Complex>& data) {
    const size_t n = data.size();
    if (n < 2) return;

    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const Complex tmp = data[i];
            data[i] = data[j];
            data[j] = tmp;
        }
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const Complex wn{static_cast<float>(std::cos(angle)),
                         static_cast<float>(std::sin(angle))};
        for (size_t i = 0; i < n; i += len) {
            Complex w{1.0f, 0.0f};
            for (size_t k = 0; k < len / 2; ++k) {
                const Complex a = data[i + k];
                const Complex b = data[i + k + len / 2];
                const Complex t{w.re * b.re - w.im * b.im,
                                w.re * b.im + w.im * b.re};
                data[i + k] = {a.re + t.re, a.im + t.im};
                data[i + k + len / 2] = {a.re - t.re, a.im - t.im};
                w = {w.re * wn.re - w.im * wn.im, w.re * wn.im + w.im * wn.re};
            }
        }
    }
}

std::vector<float> magnitude_spectrum(const float* samples, size_t count,
                                      size_t n) {
    std::vector<Complex> data(n);
    for (size_t i = 0; i < n; ++i) {
        const float s = i < count ? samples[i] : 0.0f;
        // Hann window.
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(
                                          2.0 * kPi * i / (n - 1)));
        data[i].re = s * w;
    }
    fft(data);
    std::vector<float> mags(n / 2 + 1);
    for (size_t i = 0; i < mags.size(); ++i)
        mags[i] = std::sqrt(data[i].re * data[i].re + data[i].im * data[i].im);
    return mags;
}

}  // namespace looks::mod
