// The reference walk below stays verbatim: it is the equivalence check.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "doc/effect_instance.h"
#include "gfx/error_diffusion.h"
#include "test_framework.h"

using namespace looks;

namespace {

float ref_eotf(float x) {
    return x <= 0.04045f ? x / 12.92f
                         : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

void ref_hilbert_d2xy(int order, uint64_t d, uint32_t* out_x,
                      uint32_t* out_y) {
    uint32_t x = 0, y = 0;
    for (int s = 0; s < order; ++s) {
        const uint32_t rx = 1u & static_cast<uint32_t>(d >> 1);
        const uint32_t ry = 1u & static_cast<uint32_t>(d ^ rx);
        if (ry == 0) {
            if (rx == 1) {
                x = (1u << s) - 1u - x;
                y = (1u << s) - 1u - y;
            }
            const uint32_t t = x;
            x = y;
            y = t;
        }
        x += rx << s;
        y += ry << s;
        d >>= 2;
    }
    *out_x = x;
    *out_y = y;
}

struct RefState {
    std::vector<uint32_t> out;   // packed 3x5-bit picks, like EdState
    std::vector<float> carry;    // interleaved RGB, like the original
};

void ref_error_diffusion(const uint16_t* halves, uint32_t width,
                         uint32_t height, const doc::EffectInstance& fx,
                         RefState& slot) {
    const size_t n = static_cast<size_t>(width) * height;
    const float levels = std::clamp(fx.params[0], 2.0f, 16.0f);
    const int kernel =
        static_cast<int>(std::clamp(fx.params[1], 0.0f, 7.0f) + 0.5f);
    const bool serp = fx.params[2] >= 0.5f;
    const float carry_amt = std::clamp(fx.params[3], 0.0f, 1.0f);
    const float steps = levels - 1.0f;

    static const std::vector<float>& lin_lut = [] {
        static std::vector<float> lut(65536);
        for (uint32_t h16 = 0; h16 < 65536; ++h16)
            lut[h16] = std::clamp(
                gfx::half_to_float(static_cast<uint16_t>(h16)), 0.0f, 1.0f);
        return lut;
    }();

    std::vector<float> work(n * 3);
    float* const v = work.data();
    for (size_t i = 0; i < n; ++i)
        for (size_t c = 0; c < 3; ++c)
            v[i * 3 + c] = lin_lut[halves[i * 4 + c]];
    if (carry_amt > 0.0f && slot.carry.size() == n * 3)
        for (size_t i = 0; i < n * 3; ++i) v[i] += slot.carry[i] * carry_amt;
    std::vector<float> next_carry;
    if (carry_amt > 0.0f) next_carry.assign(n * 3, 0.0f);

    struct Tap {
        int dx, dy;
        float wgt;
    };
    static constexpr Tap kFs[4] = {{1, 0, 7.0f / 16.0f},
                                   {-1, 1, 3.0f / 16.0f},
                                   {0, 1, 5.0f / 16.0f},
                                   {1, 1, 1.0f / 16.0f}};
    static constexpr Tap kAtk[6] = {{1, 0, 1.0f / 8.0f}, {2, 0, 1.0f / 8.0f},
                                    {-1, 1, 1.0f / 8.0f}, {0, 1, 1.0f / 8.0f},
                                    {1, 1, 1.0f / 8.0f}, {0, 2, 1.0f / 8.0f}};
    static constexpr Tap kJarvis[12] = {
        {1, 0, 7.0f / 48.0f},  {2, 0, 5.0f / 48.0f},  {-2, 1, 3.0f / 48.0f},
        {-1, 1, 5.0f / 48.0f}, {0, 1, 7.0f / 48.0f},  {1, 1, 5.0f / 48.0f},
        {2, 1, 3.0f / 48.0f},  {-2, 2, 1.0f / 48.0f}, {-1, 2, 3.0f / 48.0f},
        {0, 2, 5.0f / 48.0f},  {1, 2, 3.0f / 48.0f},  {2, 2, 1.0f / 48.0f}};
    static constexpr Tap kStucki[12] = {
        {1, 0, 8.0f / 42.0f},  {2, 0, 4.0f / 42.0f},  {-2, 1, 2.0f / 42.0f},
        {-1, 1, 4.0f / 42.0f}, {0, 1, 8.0f / 42.0f},  {1, 1, 4.0f / 42.0f},
        {2, 1, 2.0f / 42.0f},  {-2, 2, 1.0f / 42.0f}, {-1, 2, 2.0f / 42.0f},
        {0, 2, 4.0f / 42.0f},  {1, 2, 2.0f / 42.0f},  {2, 2, 1.0f / 42.0f}};
    static constexpr Tap kBurkes[7] = {
        {1, 0, 8.0f / 32.0f},  {2, 0, 4.0f / 32.0f}, {-2, 1, 2.0f / 32.0f},
        {-1, 1, 4.0f / 32.0f}, {0, 1, 8.0f / 32.0f}, {1, 1, 4.0f / 32.0f},
        {2, 1, 2.0f / 32.0f}};
    static constexpr Tap kSierra[10] = {
        {1, 0, 5.0f / 32.0f},  {2, 0, 3.0f / 32.0f}, {-2, 1, 2.0f / 32.0f},
        {-1, 1, 4.0f / 32.0f}, {0, 1, 5.0f / 32.0f}, {1, 1, 4.0f / 32.0f},
        {2, 1, 3.0f / 32.0f},  {-1, 2, 2.0f / 32.0f}, {0, 2, 3.0f / 32.0f},
        {1, 2, 2.0f / 32.0f}};
    const Tap* taps = kFs;
    int ntaps = 4;
    switch (kernel) {
        case 1: taps = kAtk; ntaps = 6; break;
        case 2: taps = kJarvis; ntaps = 12; break;
        case 3: taps = kStucki; ntaps = 12; break;
        case 4: taps = kBurkes; ntaps = 7; break;
        case 5: taps = kSierra; ntaps = 10; break;
        default: break;
    }

    const int nlevels_q =
        std::min(17, static_cast<int>(std::lround(std::ceil(steps))) + 1);
    float level_lin[17];
    float thresh_lin[16];
    for (int k = 0; k < nlevels_q; ++k)
        level_lin[k] = ref_eotf(
            std::clamp(static_cast<float>(k) / steps, 0.0f, 1.0f));
    for (int k = 0; k + 1 < nlevels_q; ++k)
        thresh_lin[k] = ref_eotf(std::clamp(
            (static_cast<float>(k) + 0.5f) / steps, 0.0f, 1.0f));

    const int iw = static_cast<int>(width);
    const int ih = static_cast<int>(height);

    if (kernel == 6) {
        static constexpr int16_t kOstro[128][3] = {
            {13, 0, 5},      {13, 0, 5},      {21, 0, 10},
            {7, 0, 4},       {8, 0, 5},       {47, 3, 28},
            {23, 3, 13},     {15, 3, 8},      {22, 6, 11},
            {43, 15, 20},    {7, 3, 3},       {501, 224, 211},
            {249, 116, 103}, {165, 80, 67},   {123, 62, 49},
            {489, 256, 191}, {81, 44, 31},    {483, 272, 181},
            {60, 35, 22},    {53, 32, 19},    {237, 148, 83},
            {471, 304, 161}, {3, 2, 1},       {481, 314, 185},
            {354, 226, 155}, {1389, 866, 685},{227, 138, 125},
            {267, 158, 163}, {327, 188, 220}, {61, 34, 45},
            {627, 338, 505}, {1227, 638, 1075},{20, 10, 19},
            {1937, 1000, 1767},{977, 520, 855},{657, 360, 551},
            {71, 40, 57},    {2005, 1160, 1539},{337, 200, 247},
            {2039, 1240, 1425},{257, 160, 171},{691, 440, 437},
            {1045, 680, 627},{301, 200, 171}, {177, 120, 95},
            {2141, 1480, 1083},{1079, 760, 513},{725, 520, 323},
            {137, 100, 57},  {2209, 1640, 855},{53, 40, 19},
            {2243, 1720, 741},{565, 440, 171},{759, 600, 209},
            {1147, 920, 285},{2311, 1880, 513},{97, 80, 19},
            {335, 280, 57},  {1181, 1000, 171},{793, 680, 95},
            {599, 520, 57},  {2413, 2120, 171},{405, 360, 19},
            {2447, 2200, 57},{11, 10, 0},     {158, 151, 3},
            {178, 179, 7},   {1030, 1091, 63},{248, 277, 21},
            {318, 375, 35},  {458, 571, 63},  {878, 1159, 147},
            {5, 7, 1},       {172, 181, 37},  {97, 76, 22},
            {72, 41, 17},    {119, 47, 29},   {4, 1, 1},
            {4, 1, 1},       {4, 1, 1},       {4, 1, 1},
            {4, 1, 1},       {4, 1, 1},       {4, 1, 1},
            {4, 1, 1},       {4, 1, 1},       {65, 18, 17},
            {95, 29, 26},    {185, 62, 53},   {30, 11, 9},
            {35, 14, 11},    {85, 37, 28},    {55, 26, 19},
            {80, 41, 29},    {155, 86, 59},   {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {5, 3, 2},       {5, 3, 2},       {5, 3, 2},
            {305, 176, 119}, {155, 86, 59},   {105, 56, 39},
            {80, 41, 29},    {65, 32, 23},    {55, 26, 19},
            {335, 152, 113}, {85, 37, 28},    {115, 48, 37},
            {35, 14, 11},    {355, 136, 109}, {30, 11, 9},
            {365, 128, 107}, {185, 62, 53},   {25, 8, 7},
            {95, 29, 26},    {385, 112, 103}, {65, 18, 17},
            {395, 104, 101}, {4, 1, 1},
        };
        float ostro_w[128][3];
        for (int r = 0; r < 128; ++r) {
            const float m = static_cast<float>(
                kOstro[r][0] + kOstro[r][1] + kOstro[r][2]);
            for (int t = 0; t < 3; ++t)
                ostro_w[r][t] = static_cast<float>(kOstro[r][t]) / m;
        }
        uint8_t row_lut[4096];
        {
            int b = 0;
            for (int j = 0; j < 4096; ++j) {
                const float vj = (static_cast<float>(j) + 0.5f) / 4096.0f;
                while (b < 255 &&
                       vj > ref_eotf((static_cast<float>(b) + 0.5f) / 255.0f))
                    ++b;
                row_lut[j] = static_cast<uint8_t>(b < 128 ? b : 255 - b);
            }
        }
        for (int y = 0; y < ih; ++y) {
            const bool reverse = serp && ((y & 1) != 0);
            const int dir = reverse ? -1 : 1;
            const bool has_down = y + 1 < ih;
            for (int xi = 0; xi < iw; ++xi) {
                const int x = reverse ? (iw - 1 - xi) : xi;
                const size_t i = (static_cast<size_t>(y) * width +
                                  static_cast<size_t>(x)) * 3;
                const int xn = x + dir;
                const int xb = x - dir;
                const bool has_next = xn >= 0 && xn < iw;
                const bool has_back = xb >= 0 && xb < iw;
                for (size_t c = 0; c < 3; ++c) {
                    const float orig =
                        lin_lut[halves[(static_cast<size_t>(y) * width +
                                        static_cast<size_t>(x)) * 4 + c]];
                    const float* wv = ostro_w[row_lut[std::min(
                        4095, static_cast<int>(orig * 4096.0f))]];
                    const float clamped = std::clamp(v[i + c], 0.0f, 1.0f);
                    int k = 0;
                    while (k + 1 < nlevels_q && clamped > thresh_lin[k]) ++k;
                    const float err = clamped - level_lin[k];
                    v[i + c] = static_cast<float>(k);
                    if (has_next)
                        v[(static_cast<size_t>(y) * width +
                           static_cast<size_t>(xn)) * 3 + c] += err * wv[0];
                    if (has_down) {
                        if (has_back)
                            v[(static_cast<size_t>(y + 1) * width +
                               static_cast<size_t>(xb)) * 3 + c] +=
                                err * wv[1];
                        v[(static_cast<size_t>(y + 1) * width +
                           static_cast<size_t>(x)) * 3 + c] += err * wv[2];
                    }
                    if (carry_amt > 0.0f) next_carry[i + c] = err;
                }
            }
        }
    } else if (kernel == 7) {
        int order = 0;
        while ((1u << order) < width || (1u << order) < height) ++order;
        float wts[16];
        const float decay = std::exp(std::log(16.0f) / 15.0f);
        for (int a = 0; a < 16; ++a)
            wts[a] = std::pow(decay, -static_cast<float>(a));
        float ring[16][3] = {};
        int head = 0;
        const uint64_t total = uint64_t{1} << (2 * order);
        for (uint64_t d = 0; d < total; ++d) {
            uint32_t hx = 0, hy = 0;
            ref_hilbert_d2xy(order, d, &hx, &hy);
            if (hx >= width || hy >= height) continue;
            const size_t i = (static_cast<size_t>(hy) * width + hx) * 3;
            float errs[3];
            for (size_t c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (int a = 0; a < 16; ++a)
                    sum += ring[(head + 15 - a) & 15][c] * wts[a];
                const float clamped = std::clamp(v[i + c], 0.0f, 1.0f);
                const float nudged = clamped + sum;
                int k = 0;
                while (k + 1 < nlevels_q && nudged > thresh_lin[k]) ++k;
                errs[c] = clamped - level_lin[k];
                v[i + c] = static_cast<float>(k);
                if (carry_amt > 0.0f) next_carry[i + c] = errs[c];
            }
            for (size_t c = 0; c < 3; ++c) ring[head][c] = errs[c];
            head = (head + 1) & 15;
        }
    } else {
        for (int y = 0; y < ih; ++y) {
            const bool reverse = serp && ((y & 1) != 0);
            const int dir = reverse ? -1 : 1;
            for (int xi = 0; xi < iw; ++xi) {
                const int x = reverse ? (iw - 1 - xi) : xi;
                const size_t i = (static_cast<size_t>(y) * width +
                                  static_cast<size_t>(x)) * 3;
                for (size_t c = 0; c < 3; ++c) {
                    const float clamped = std::clamp(v[i + c], 0.0f, 1.0f);
                    int k = 0;
                    while (k + 1 < nlevels_q && clamped > thresh_lin[k]) ++k;
                    const float err = clamped - level_lin[k];
                    v[i + c] = static_cast<float>(k);
                    for (int t = 0; t < ntaps; ++t) {
                        const int nx = x + taps[t].dx * dir;
                        const int ny = y + taps[t].dy;
                        if (nx < 0 || ny < 0 || nx >= iw || ny >= ih)
                            continue;
                        v[(static_cast<size_t>(ny) * width +
                           static_cast<size_t>(nx)) * 3 + c] +=
                            err * taps[t].wgt;
                    }
                    if (carry_amt > 0.0f) next_carry[i + c] = err;
                }
            }
        }
    }
    if (carry_amt > 0.0f) slot.carry = std::move(next_carry);
    else slot.carry.clear();

    slot.out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t packed = 0;
        for (size_t c = 0; c < 3; ++c) {
            const int k = std::clamp(
                static_cast<int>(v[i * 3 + c] + 0.5f), 0, nlevels_q - 1);
            packed |= static_cast<uint32_t>(k) << (5 * c);
        }
        slot.out[i] = packed;
    }
}

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// The test needs this encoder: production only decodes halves.
uint16_t float_to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp =
        static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu); // max half
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                 (man >> 13));
}

std::vector<uint16_t> make_halves(uint32_t w, uint32_t h, uint32_t seed) {
    std::vector<uint16_t> halves(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < 4; ++c) {
                const float f =
                    static_cast<float>(hash32(seed ^ (y * w + x) * 4u ^ c) &
                                       0xFFFF) /
                    65535.0f;
                halves[(static_cast<size_t>(y) * w + x) * 4 + c] =
                    float_to_half(f);
            }
    return halves;
}

}  // namespace

TEST(error_diffusion_matches_reference) {
    // Only exact mode matches: fast mode uses a different banded walk.
    // The odd width 97 exercises the margin and edge paths of each kernel.
    const uint32_t w = 97, h = 200;
    const std::vector<uint16_t> frame_a = make_halves(w, h, 11);
    const std::vector<uint16_t> frame_b = make_halves(w, h, 12);
    for (int kernel = 0; kernel <= 7; ++kernel) {
        for (int serp = 0; serp <= 1; ++serp) {
            for (const float lv : {2.0f, 5.0f}) {
                doc::EffectInstance fx;
                fx.params = {lv, static_cast<float>(kernel),
                             static_cast<float>(serp), 0.6f, 0.0f};
                gfx::EdState opt;
                RefState ref;
                for (const auto* frame : {&frame_a, &frame_b}) {
                    gfx::run_error_diffusion(frame->data(), w, h, fx, opt);
                    ref_error_diffusion(frame->data(), w, h, fx, ref);
                    CHECK(opt.out == ref.out);
                }
            }
        }
    }
}

TEST(error_diffusion_fast_mode) {
    const uint32_t w = 97, h = 61;
    const std::vector<uint16_t> frame = make_halves(w, h, 21);
    for (const int kernel : {0, 6, 7}) {
        doc::EffectInstance fx;
        fx.params = {2.0f, static_cast<float>(kernel), 1.0f, 0.0f, 1.0f};
        gfx::EdState a, b;
        gfx::run_error_diffusion(frame.data(), w, h, fx, a);
        gfx::run_error_diffusion(frame.data(), w, h, fx, b);
        CHECK(a.out == b.out);
        doc::EffectInstance exact = fx;
        exact.params[4] = 0.0f;
        gfx::EdState e;
        gfx::run_error_diffusion(frame.data(), w, h, exact, e);
        CHECK(a.out != e.out);
    }
}
