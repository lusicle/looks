#include "gfx/error_diffusion.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "codec/core.h"
#include "doc/effect_instance.h"
#include "util/color.h"

namespace looks::gfx {

namespace {

void hilbert_d2xy(int order, uint64_t d, uint32_t* out_x, uint32_t* out_y) {
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

}  // namespace

// Kernel ids are frozen: 0-5 raster taps, 6 Ostromoukhov, 7 Riemersma.
// Diffusion runs in linear light; accumulation order fixes the exact result.
void run_error_diffusion(const uint16_t* halves, uint32_t width,
                         uint32_t height, const doc::EffectInstance& fx,
                         EdState& slot) {
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
                half_to_float(static_cast<uint16_t>(h16)), 0.0f, 1.0f);
        return lut;
    }();

    slot.work.resize(n * 3);
    slot.idx.resize(n * 3);
    float* const v = slot.work.data();
    uint8_t* const picks = slot.idx.data();
    codec::parallel_blocks(
        static_cast<int>(height), true, [&](int begin, int end) {
            for (int row = begin; row < end; ++row) {
                const size_t base = static_cast<size_t>(row) * width;
                for (size_t x = 0; x < width; ++x)
                    for (size_t c = 0; c < 3; ++c)
                        v[c * n + base + x] =
                            lin_lut[halves[(base + x) * 4 + c]];
            }
        });
    if (carry_amt > 0.0f && slot.carry.size() == n * 3)
        for (size_t i = 0; i < n * 3; ++i) v[i] += slot.carry[i] * carry_amt;
    std::vector<float> next_carry;
    if (carry_amt > 0.0f) next_carry.assign(n * 3, 0.0f);

    const int iw = static_cast<int>(width);
    const int ih = static_cast<int>(height);
    // fast: 16 bands per channel; error drops at each seam like a frame edge.
    // Band count must depend only on the image, never on the machine.
    const bool fast = fx.params.size() > 4 ? fx.params[4] >= 0.5f : true;
    const int bands = fast ? std::min(16, ih) : 1;
    const int band_rows = (ih + bands - 1) / bands;
    const auto run_band_tasks =
        [&](const std::function<void(int, int, int)>& walk) {
            codec::parallel_tasks(3 * bands, [&](int t) {
                const int c = t / bands;
                const int band = t % bands;
                const int y0 = band * band_rows;
                const int y1 = std::min(ih, y0 + band_rows);
                if (y0 < y1) walk(c, y0, y1);
            });
        };

    // Tap order is fixed: float accumulation order defines the result.
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
    int margin = 1;
    switch (kernel) {
        case 1: taps = kAtk; ntaps = 6; margin = 2; break;
        case 2: taps = kJarvis; ntaps = 12; margin = 2; break;
        case 3: taps = kStucki; ntaps = 12; margin = 2; break;
        case 4: taps = kBurkes; ntaps = 7; margin = 2; break;
        case 5: taps = kSierra; ntaps = 10; margin = 2; break;
        default: break;
    }

    // Thresholds are encoded midpoints, so the count gives nearest-in-encoded.
    float level_lin[17];
    float thresh_lin[16];
    const int nlevels_q = ed_level_table(fx, level_lin);
    for (int k = 0; k + 1 < nlevels_q; ++k)
        thresh_lin[k] = color::srgb_eotf(std::clamp(
            (static_cast<float>(k) + 0.5f) / steps, 0.0f, 1.0f));

    if (kernel == 6) {
        // kOstro rows cover encoded levels 0..127; level i above mirrors 255-i.
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
        // row_lut maps linear values to encoded rows; darkest rows approximate.
        uint8_t row_lut[4096];
        {
            int b = 0;
            for (int j = 0; j < 4096; ++j) {
                const float vj = (static_cast<float>(j) + 0.5f) / 4096.0f;
                while (b < 255 &&
                       vj > color::srgb_eotf(
                                (static_cast<float>(b) + 0.5f) / 255.0f))
                    ++b;
                row_lut[j] = static_cast<uint8_t>(b < 128 ? b : 255 - b);
            }
        }
        // below is null at a band or frame edge; the down taps drop there.
        const auto ostro_row = [&](int c, float* row, float* below, int y,
                                   float* ncp) {
            const bool reverse = serp && ((y & 1) != 0);
            const int dir = reverse ? -1 : 1;
            const int x0 = reverse ? iw - 1 : 0;
            float acc1 = row[x0];
            for (int xi = 0; xi < iw; ++xi) {
                const int x = reverse ? (iw - 1 - xi) : xi;
                const int xb = x - dir;
                const bool has_back = xb >= 0 && xb < iw;
                // Weights index the original input level, not the shift.
                const float orig =
                    lin_lut[halves[(static_cast<size_t>(y) * width +
                                    static_cast<size_t>(x)) * 4 + c]];
                const float* wv = ostro_w[row_lut[std::min(
                    4095, static_cast<int>(orig * 4096.0f))]];
                // The unclamped pick gives the same count; only error clamps.
                int k = 0;
                for (int j = 0; j + 1 < nlevels_q; ++j)
                    k += acc1 > thresh_lin[j] ? 1 : 0;
                const float clamped = std::clamp(acc1, 0.0f, 1.0f);
                const float err = clamped - level_lin[k];
                picks[static_cast<size_t>(c) * n +
                      static_cast<size_t>(y) * width +
                      static_cast<size_t>(x)] = static_cast<uint8_t>(k);
                if (xi + 1 < iw) acc1 = row[x + dir] + err * wv[0];
                if (below) {
                    if (has_back) below[xb] += err * wv[1];
                    below[x] += err * wv[2];
                }
                if (ncp)
                    ncp[static_cast<size_t>(y) * width +
                        static_cast<size_t>(x)] = err;
            }
        };
        run_band_tasks([&](int c, int y0, int y1) {
            float* const vp = v + static_cast<size_t>(c) * n;
            float* const ncp = carry_amt > 0.0f
                ? next_carry.data() + static_cast<size_t>(c) * n
                : nullptr;
            for (int y = y0; y < y1; ++y)
                ostro_row(c, vp + static_cast<size_t>(y) * width,
                          y + 1 < y1
                              ? vp + static_cast<size_t>(y + 1) * width
                              : nullptr,
                          y, ncp);
        });
    } else if (kernel == 7) {
        // The serpentine flag has no meaning on a space-filling path.
        if (slot.hilbert_w != width || slot.hilbert_h != height) {
            int order = 0;
            while ((1u << order) < width || (1u << order) < height) ++order;
            slot.hilbert.clear();
            slot.hilbert.reserve(n);
            const uint64_t total = uint64_t{1} << (2 * order);
            for (uint64_t d = 0; d < total; ++d) {
                uint32_t hx = 0, hy = 0;
                hilbert_d2xy(order, d, &hx, &hy);
                if (hx >= width || hy >= height) continue;
                slot.hilbert.push_back(hy * width + hx);
            }
            slot.hilbert_w = width;
            slot.hilbert_h = height;
        }
        float wts[16];
        const float decay = std::exp(std::log(16.0f) / 15.0f);
        for (int a = 0; a < 16; ++a)
            wts[a] = std::pow(decay, -static_cast<float>(a));
        // fast splits the walk; segment count depends only on the image.
        const size_t visits = slot.hilbert.size();
        const size_t seg_len = (visits + bands - 1) / bands;
        codec::parallel_tasks(3 * bands, [&](int t) {
            const int c = t / bands;
            const size_t s0 = static_cast<size_t>(t % bands) * seg_len;
            const size_t s1 = std::min(visits, s0 + seg_len);
            if (s0 >= s1) return;
            float* const vp = v + static_cast<size_t>(c) * n;
            float* const nc = carry_amt > 0.0f
                ? next_carry.data() + static_cast<size_t>(c) * n
                : nullptr;
            float ring[16] = {};
            int head = 0;
            for (size_t s = s0; s < s1; ++s) {
                const uint32_t pi = slot.hilbert[s];
                float sum = 0.0f;
                for (int a = 0; a < 16; ++a)
                    sum += ring[(head + 15 - a) & 15] * wts[a];
                const float clamped = std::clamp(vp[pi], 0.0f, 1.0f);
                const float nudged = clamped + sum;
                int k = 0;
                for (int j = 0; j + 1 < nlevels_q; ++j)
                    k += nudged > thresh_lin[j] ? 1 : 0;
                const float err = clamped - level_lin[k];
                picks[static_cast<size_t>(c) * n + pi] =
                    static_cast<uint8_t>(k);
                if (nc) nc[pi] = err;
                ring[head] = err;
                head = (head + 1) & 15;
            }
        });
    } else {
        // Every tap table must lead with its same-row taps.
        const int n_same = ntaps >= 2 && taps[1].dy == 0 ? 2 : 1;
        const float w1 = taps[0].wgt;
        const float w2 = n_same == 2 ? taps[1].wgt : 0.0f;
        // clip_rows bounds the below taps in dy; bands shrink it at their edge.
        const auto raster_row = [&](int c, float* row, int y, int clip_rows,
                                    float* ncp) {
            const bool reverse = serp && ((y & 1) != 0);
            const int dir = reverse ? -1 : 1;
            ptrdiff_t delta[12];
            for (int t = n_same; t < ntaps; ++t)
                delta[t] = static_cast<ptrdiff_t>(taps[t].dy) * iw +
                           taps[t].dx * dir;
            const bool rows_ok = clip_rows >= margin;
            const int x0 = reverse ? iw - 1 : 0;
            float acc1 = row[x0];
            float acc2 = iw > 1 ? row[x0 + dir] : 0.0f;
            for (int xi = 0; xi < iw; ++xi) {
                const int x = reverse ? (iw - 1 - xi) : xi;
                const bool interior =
                    rows_ok && x >= margin && x < iw - margin;
                // Pick unclamped; the error must use the clamped value.
                int k = 0;
                for (int j = 0; j + 1 < nlevels_q; ++j)
                    k += acc1 > thresh_lin[j] ? 1 : 0;
                const float clamped = std::clamp(acc1, 0.0f, 1.0f);
                const float err = clamped - level_lin[k];
                picks[static_cast<size_t>(c) * n +
                      static_cast<size_t>(y) * width +
                      static_cast<size_t>(x)] = static_cast<uint8_t>(k);
                if (xi + 1 < iw) acc1 = acc2 + err * w1;
                if (xi + 2 < iw) {
                    acc2 = row[static_cast<ptrdiff_t>(x) + 2 * dir];
                    if (n_same == 2) acc2 += err * w2;
                } else {
                    acc2 = 0.0f;
                }
                if (interior) {
                    for (int t = n_same; t < ntaps; ++t)
                        row[static_cast<ptrdiff_t>(x) + delta[t]] +=
                            err * taps[t].wgt;
                } else {
                    for (int t = n_same; t < ntaps; ++t) {
                        const int nx = x + taps[t].dx * dir;
                        if (nx < 0 || nx >= iw || taps[t].dy > clip_rows)
                            continue;
                        row[static_cast<ptrdiff_t>(nx) +
                            static_cast<ptrdiff_t>(taps[t].dy) * iw] +=
                            err * taps[t].wgt;
                    }
                }
                if (ncp)
                    ncp[static_cast<size_t>(y) * width +
                        static_cast<size_t>(x)] = err;
            }
        };
        run_band_tasks([&](int c, int y0, int y1) {
            float* const vp = v + static_cast<size_t>(c) * n;
            float* const ncp = carry_amt > 0.0f
                ? next_carry.data() + static_cast<size_t>(c) * n
                : nullptr;
            for (int y = y0; y < y1; ++y)
                raster_row(c, vp + static_cast<size_t>(y) * width, y,
                           std::min(2, y1 - 1 - y), ncp);
        });
    }
    if (carry_amt > 0.0f) slot.carry = std::move(next_carry);
    else slot.carry.clear();

    slot.out.resize(n);
    codec::parallel_blocks(
        static_cast<int>(height), true, [&](int begin, int end) {
            for (int row = begin; row < end; ++row) {
                const size_t base = static_cast<size_t>(row) * width;
                for (size_t x = 0; x < width; ++x) {
                    const size_t i = base + x;
                    slot.out[i] =
                        static_cast<uint32_t>(picks[i]) |
                        (static_cast<uint32_t>(picks[n + i]) << 5) |
                        (static_cast<uint32_t>(picks[2 * n + i]) << 10);
                }
            }
        });
}

int ed_level_table(const doc::EffectInstance& fx, float out_levels[17]) {
    const float levels = std::clamp(fx.params[0], 2.0f, 16.0f);
    const float steps = levels - 1.0f;
    const int nlevels_q =
        std::min(17, static_cast<int>(std::lround(std::ceil(steps))) + 1);
    for (int k = 0; k < nlevels_q; ++k)
        out_levels[k] = color::srgb_eotf(
            std::clamp(static_cast<float>(k) / steps, 0.0f, 1.0f));
    for (int k = nlevels_q; k < 17; ++k)
        out_levels[k] = 0.0f;
    return nlevels_q;
}

}  // namespace looks::gfx
