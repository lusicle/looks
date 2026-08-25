#include "gfx/shape_sdf.h"

#include <algorithm>
#include <cmath>

namespace looks::gfx {

namespace {

constexpr int kSubdiv = kShapeSubdiv;
constexpr float kBig = 1.0e9f;

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

// Two-pass 3x3 chamfer sweep over an initialized cost field: min-plus
// propagation of distance-to-seed. Near the boundary the exact seeds
// dominate, so the few percent of chamfer error only lands where
// feathering hides it.
void chamfer(std::vector<float>& d, uint32_t w, uint32_t h, float step) {
    const float a = step;
    const float b = step * 1.41421356f;
    auto at = [&](uint32_t x, uint32_t y) -> float& { return d[y * w + x]; };
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float v = at(x, y);
            if (x > 0) v = std::min(v, at(x - 1, y) + a);
            if (y > 0) {
                v = std::min(v, at(x, y - 1) + a);
                if (x > 0) v = std::min(v, at(x - 1, y - 1) + b);
                if (x + 1 < w) v = std::min(v, at(x + 1, y - 1) + b);
            }
            at(x, y) = v;
        }
    for (uint32_t y = h; y-- > 0;)
        for (uint32_t x = w; x-- > 0;) {
            float v = at(x, y);
            if (x + 1 < w) v = std::min(v, at(x + 1, y) + a);
            if (y + 1 < h) {
                v = std::min(v, at(x, y + 1) + a);
                if (x + 1 < w) v = std::min(v, at(x + 1, y + 1) + b);
                if (x > 0) v = std::min(v, at(x - 1, y + 1) + b);
            }
            at(x, y) = v;
        }
}

void encode(const std::vector<float>& d, std::vector<uint8_t>* out) {
    out->resize(d.size());
    const float inv = 1.0f / (2.0f * kShapeSdfRange);
    for (size_t i = 0; i < d.size(); ++i) {
        const float v = std::clamp(0.5f + d[i] * inv, 0.0f, 1.0f);
        (*out)[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
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
                      std::vector<uint8_t>* out) {
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
    const float step = std::max(sx, sy);

    if (closed && path.size() >= 3) {
        // Coverage per texel from a 2x2 winding supersample (nonzero
        // rule), scanline crossings per subrow.
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
        // Seeds: partial texels carry exact-ish subpixel distances; the
        // sweep fills the far field both ways.
        std::vector<float> dpos(static_cast<size_t>(w) * h, kBig);
        std::vector<float> dneg(static_cast<size_t>(w) * h, kBig);
        for (size_t i = 0; i < cov.size(); ++i) {
            if (cov[i] == 0 || cov[i] == 4) continue;
            const float c = static_cast<float>(cov[i]) * 0.25f;
            dpos[i] = std::max(0.5f - c, 0.0f) * step;
            dneg[i] = std::max(c - 0.5f, 0.0f) * step;
        }
        chamfer(dpos, w, h, step);
        chamfer(dneg, w, h, step);
        for (size_t i = 0; i < d.size(); ++i) {
            const float c = static_cast<float>(cov[i]) * 0.25f;
            if (cov[i] != 0 && cov[i] != 4)
                d[i] = (0.5f - c) * step;
            else if (cov[i] == 0)
                d[i] = dpos[i];
            else
                d[i] = -dneg[i];
        }
    } else {
        // Open stroke: exact distance in a band around each segment,
        // chamfer fills the rest.
        const int pad = 2;
        for (const Seg& s : segs) {
            const float x0m = std::min(s.x0, s.x1), x1m = std::max(s.x0, s.x1);
            const float y0m = std::min(s.y0, s.y1), y1m = std::max(s.y0, s.y1);
            const int tx0 = std::max(
                0, static_cast<int>(std::floor(x0m / sx)) - pad);
            const int tx1 = std::min(
                static_cast<int>(w) - 1,
                static_cast<int>(std::floor(x1m / sx)) + pad);
            const int ty0 = std::max(
                0, static_cast<int>(std::floor(y0m / sy)) - pad);
            const int ty1 = std::min(
                static_cast<int>(h) - 1,
                static_cast<int>(std::floor(y1m / sy)) + pad);
            for (int y = ty0; y <= ty1; ++y)
                for (int x = tx0; x <= tx1; ++x) {
                    const float px = (static_cast<float>(x) + 0.5f) * sx;
                    const float py = (static_cast<float>(y) + 0.5f) * sy;
                    float& cell =
                        d[static_cast<size_t>(y) * w +
                          static_cast<size_t>(x)];
                    cell = std::min(cell, seg_dist(s, px, py));
                }
        }
        chamfer(d, w, h, step);
    }
    encode(d, out);
}

}  // namespace looks::gfx
