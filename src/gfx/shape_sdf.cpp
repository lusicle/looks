#include "gfx/shape_sdf.h"

#include <algorithm>
#include <cmath>

namespace looks::gfx {

namespace {

constexpr int kSubdiv = kShapeSubdiv;
constexpr float kBig = 1.0e9f;
// Exact-distance band around the polyline, texels. Inside it every
// texel carries true segment distance; outside it the Euclidean
// transform propagates from the band with error bounded by one texel -
// flat in the far field where feather gradients are shallow, so the
// feather never wobbles along the edge.
constexpr int kBand = 3;

struct Seg {
    float x0, y0, x1, y1;
};

// Exact point-to-segment distance.
float seg_dist(const Seg& s, float px, float py) {
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    const float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 1.0e-12f)
        t = std::clamp(((px - s.x0) * dx + (py - s.y0) * dy) / len2, 0.0f,
                       1.0f);
    const float cx = s.x0 + dx * t, cy = s.y0 + dy * t;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

// One squared-distance pass along a line of n samples in INDEX units
// (Felzenszwalb-Huttenlocher lower envelope of parabolas):
// out[q] = min_p ((q - p)^2 + g[p]). Exact, O(n), fixed order. No-seed
// cells carry huge-but-finite g and simply never win the envelope.
void edt_1d(const float* g, float* out, int* v, float* z, int n) {
    auto sect = [&](int q, int p) {
        return ((g[q] + static_cast<float>(q) * q) -
                (g[p] + static_cast<float>(p) * p)) /
               (2.0f * (q - p));
    };
    int k = 0;
    v[0] = 0;
    z[0] = -kBig;
    z[1] = kBig;
    for (int q = 1; q < n; ++q) {
        float s = sect(q, v[k]);
        while (k > 0 && s <= z[k]) {
            --k;
            s = sect(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kBig;
    }
    int j = 0;
    for (int q = 0; q < n; ++q) {
        while (z[j + 1] < q) ++j;
        const float dq = static_cast<float>(q - v[j]);
        out[q] = dq * dq + g[v[j]];
    }
}

// Separable exact squared EDT over an initialized metric^2 field,
// anisotropy carried by scaling each axis into index units and back.
void edt_2d(std::vector<float>& d, uint32_t w, uint32_t h, float sx,
            float sy) {
    const int n = static_cast<int>(std::max(w, h));
    std::vector<float> g(n), out(n), z(n + 1);
    std::vector<int> v(n);
    const float sx2 = sx * sx, sy2 = sy * sy;
    for (uint32_t y = 0; y < h; ++y) {
        float* row = d.data() + static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) g[x] = row[x] / sx2;
        edt_1d(g.data(), out.data(), v.data(), z.data(),
               static_cast<int>(w));
        for (uint32_t x = 0; x < w; ++x) row[x] = out[x] * sx2;
    }
    for (uint32_t x = 0; x < w; ++x) {
        for (uint32_t y = 0; y < h; ++y)
            g[y] = d[static_cast<size_t>(y) * w + x] / sy2;
        edt_1d(g.data(), out.data(), v.data(), z.data(),
               static_cast<int>(h));
        for (uint32_t y = 0; y < h; ++y)
            d[static_cast<size_t>(y) * w + x] = out[y] * sy2;
    }
}

// Exact distances stamped in a kBand-texel box around every segment;
// the far field starts as squared band values for the EDT.
void stamp_band(const std::vector<Seg>& segs, uint32_t w, uint32_t h,
                float sx, float sy, std::vector<float>* band) {
    band->assign(static_cast<size_t>(w) * h, kBig);
    for (const Seg& s : segs) {
        const float x0m = std::min(s.x0, s.x1), x1m = std::max(s.x0, s.x1);
        const float y0m = std::min(s.y0, s.y1), y1m = std::max(s.y0, s.y1);
        const int tx0 =
            std::max(0, static_cast<int>(std::floor(x0m / sx)) - kBand);
        const int tx1 =
            std::min(static_cast<int>(w) - 1,
                     static_cast<int>(std::floor(x1m / sx)) + kBand);
        const int ty0 =
            std::max(0, static_cast<int>(std::floor(y0m / sy)) - kBand);
        const int ty1 =
            std::min(static_cast<int>(h) - 1,
                     static_cast<int>(std::floor(y1m / sy)) + kBand);
        for (int y = ty0; y <= ty1; ++y)
            for (int x = tx0; x <= tx1; ++x) {
                const float px = (static_cast<float>(x) + 0.5f) * sx;
                const float py = (static_cast<float>(y) + 0.5f) * sy;
                float& cell = (*band)[static_cast<size_t>(y) * w +
                                      static_cast<size_t>(x)];
                cell = std::min(cell, seg_dist(s, px, py));
            }
    }
}

void encode(const std::vector<float>& d, std::vector<uint16_t>* out) {
    out->resize(d.size());
    const float inv = 1.0f / (2.0f * kShapeSdfRange);
    for (size_t i = 0; i < d.size(); ++i) {
        const float v = std::clamp(0.5f + d[i] * inv, 0.0f, 1.0f);
        (*out)[i] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
    }
}

}  // namespace

void shape_flatten(const std::vector<doc::PathPoint>& path, bool closed,
                   float aspect, std::vector<float>* out_xy) {
    out_xy->clear();
    const size_t n = path.size();
    if (n < 2) return;
    const size_t spans = closed ? n : n - 1;
    out_xy->reserve((spans * kSubdiv + 1) * 2);
    auto emit = [&](float x, float y) {
        out_xy->push_back(x * aspect);
        out_xy->push_back(y);
    };
    emit(path[0].ax, path[0].ay);
    for (size_t i = 0; i < spans; ++i) {
        const doc::PathPoint& p0 = path[i];
        const doc::PathPoint& p3 = path[(i + 1) % n];
        const float c0x = p0.ax, c0y = p0.ay;
        const float c1x = p0.ax + p0.out_dx, c1y = p0.ay + p0.out_dy;
        const float c2x = p3.ax + p3.in_dx, c2y = p3.ay + p3.in_dy;
        const float c3x = p3.ax, c3y = p3.ay;
        for (int s = 1; s <= kSubdiv; ++s) {
            const float t = static_cast<float>(s) / kSubdiv;
            const float u = 1.0f - t;
            const float w0 = u * u * u;
            const float w1 = 3.0f * u * u * t;
            const float w2 = 3.0f * u * t * t;
            const float w3 = t * t * t;
            emit(w0 * c0x + w1 * c1x + w2 * c2x + w3 * c3x,
                 w0 * c0y + w1 * c1y + w2 * c2y + w3 * c3y);
        }
    }
}

void shape_sdf_raster(const std::vector<doc::PathPoint>& path, bool closed,
                      float aspect, uint32_t w, uint32_t h,
                      std::vector<uint16_t>* out) {
    std::vector<float> d(static_cast<size_t>(w) * h, kBig);
    std::vector<float> poly;
    shape_flatten(path, closed, aspect, &poly);
    const size_t npts = poly.size() / 2;
    if (npts < 2 || w == 0 || h == 0) {
        encode(d, out);
        return;
    }
    std::vector<Seg> segs;
    segs.reserve(npts);
    for (size_t i = 0; i + 1 < npts; ++i)
        segs.push_back({poly[i * 2], poly[i * 2 + 1], poly[i * 2 + 2],
                        poly[i * 2 + 3]});
    const float sx = aspect / static_cast<float>(w);   // texel size, metric
    const float sy = 1.0f / static_cast<float>(h);

    // Exact unsigned distance near the curve, true Euclidean transform
    // beyond it. Only texels the curve passes THROUGH seed the
    // transform (the squared form composes seed offsets as
    // sqrt(D^2 + b^2), so a fat seed offset would sag the far field -
    // a sub-texel one vanishes into it); the full exact band then
    // overrides its own ring.
    std::vector<float> band;
    stamp_band(segs, w, h, sx, sy, &band);
    const float near_thr = 1.5f * std::max(sx, sy);
    std::vector<float> field(band.size());
    for (size_t i = 0; i < band.size(); ++i)
        field[i] =
            band[i] <= near_thr ? band[i] * band[i] : kBig;
    edt_2d(field, w, h, sx, sy);
    for (size_t i = 0; i < field.size(); ++i) {
        float u = std::sqrt(field[i]);
        if (band[i] < kBig) u = std::min(u, band[i]);
        field[i] = u;
    }

    if (closed && path.size() >= 3) {
        // Sign per texel from a 2x2 winding supersample (nonzero rule),
        // scanline crossings per subrow; partial texels sit on the
        // curve and take their exact band distance with the coverage
        // sign.
        std::vector<uint8_t> cov(static_cast<size_t>(w) * h, 0);
        std::vector<std::pair<float, int>> cross;
        for (uint32_t y = 0; y < h; ++y) {
            for (int sub = 0; sub < 2; ++sub) {
                const float yy =
                    (static_cast<float>(y) + (sub ? 0.75f : 0.25f)) * sy;
                cross.clear();
                for (const Seg& s : segs) {
                    const bool up = s.y0 <= yy && s.y1 > yy;
                    const bool dn = s.y1 <= yy && s.y0 > yy;
                    if (!up && !dn) continue;
                    const float t = (yy - s.y0) / (s.y1 - s.y0);
                    cross.push_back({s.x0 + (s.x1 - s.x0) * t, up ? 1 : -1});
                }
                std::stable_sort(cross.begin(), cross.end(),
                                 [](const auto& a, const auto& b) {
                                     return a.first < b.first;
                                 });
                for (int subx = 0; subx < 2; ++subx) {
                    size_t ci = 0;
                    int wind = 0;
                    for (uint32_t x = 0; x < w; ++x) {
                        const float xx =
                            (static_cast<float>(x) +
                             (subx ? 0.75f : 0.25f)) *
                            sx;
                        while (ci < cross.size() && cross[ci].first <= xx) {
                            wind += cross[ci].second;
                            ++ci;
                        }
                        if (wind != 0) ++cov[y * w + x];
                    }
                }
            }
        }
        for (size_t i = 0; i < d.size(); ++i) {
            const float u = field[i];
            if (cov[i] == 0)
                d[i] = u;
            else if (cov[i] == 4)
                d[i] = -u;
            else
                d[i] = (0.5f - static_cast<float>(cov[i]) * 0.25f) *
                       std::min(u, std::max(sx, sy));
        }
    } else {
        // Open stroke: the unsigned field IS the answer.
        d = std::move(field);
    }
    encode(d, out);
}

}  // namespace looks::gfx
