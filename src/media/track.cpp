#include "media/track.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "util/hash.h"
#include "util/numerics.h"

namespace looks::media {

namespace {

// Fixed budgets: determinism comes from never letting timing or load
// change a count or an order.
constexpr int kMaxFeatures = 96;
constexpr int kRedetectBelow = 48;
constexpr int kRedetectEvery = 30;
constexpr int kPyrLevels = 3;
constexpr int kKltIters = 8;
constexpr int kKltWin = 3;          // 7x7 window (+-3)
constexpr float kMaxResidual = 18.0f;   // mean abs diff, 8-bit
constexpr float kMinEigen = 40.0f;
constexpr int kRansacIters = 48;
constexpr float kInlierThresh = 0.008f;  // canvas heights
constexpr uint32_t kCacheMagic = 0x334B544Cu;  // 'LTK3'

struct Pyramid {
    // Level 0 is the working base (source downsampled so the max dim
    // fits kBaseMax); each level halves.
    static constexpr uint32_t kBaseMax = 640;
    std::vector<float> img[kPyrLevels];
    uint32_t w[kPyrLevels] = {}, h[kPyrLevels] = {};

    float at(int level, int x, int y) const {
        x = std::clamp(x, 0, static_cast<int>(w[level]) - 1);
        y = std::clamp(y, 0, static_cast<int>(h[level]) - 1);
        return img[level][static_cast<size_t>(y) * w[level] +
                          static_cast<size_t>(x)];
    }
    float bilinear(int level, float x, float y) const {
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const float a = at(level, x0, y0), b = at(level, x0 + 1, y0);
        const float c = at(level, x0, y0 + 1), d = at(level, x0 + 1, y0 + 1);
        return (a * (1.0f - fx) + b * fx) * (1.0f - fy) +
               (c * (1.0f - fx) + d * fx) * fy;
    }
};

void build_pyramid(const GrayFrame& src, Pyramid* pyr) {
    // Integer decimation to the working base: pick the power-of-two
    // step that brings the max dimension under kBaseMax, box-averaged.
    uint32_t step = 1;
    while (std::max(src.width, src.height) / step > Pyramid::kBaseMax)
        step <<= 1;
    const uint32_t w0 = std::max(1u, src.width / step);
    const uint32_t h0 = std::max(1u, src.height / step);
    pyr->w[0] = w0;
    pyr->h[0] = h0;
    pyr->img[0].resize(static_cast<size_t>(w0) * h0);
    for (uint32_t y = 0; y < h0; ++y)
        for (uint32_t x = 0; x < w0; ++x) {
            uint32_t sum = 0;
            for (uint32_t sy = 0; sy < step; ++sy)
                for (uint32_t sx = 0; sx < step; ++sx)
                    sum += src.data[static_cast<size_t>(y * step + sy) *
                                        src.stride +
                                    x * step + sx];
            pyr->img[0][static_cast<size_t>(y) * w0 + x] =
                static_cast<float>(sum) / static_cast<float>(step * step);
        }
    for (int l = 1; l < kPyrLevels; ++l) {
        const uint32_t pw = pyr->w[l - 1], ph = pyr->h[l - 1];
        const uint32_t wl = std::max(1u, pw / 2), hl = std::max(1u, ph / 2);
        pyr->w[l] = wl;
        pyr->h[l] = hl;
        pyr->img[l].resize(static_cast<size_t>(wl) * hl);
        for (uint32_t y = 0; y < hl; ++y)
            for (uint32_t x = 0; x < wl; ++x)
                pyr->img[l][static_cast<size_t>(y) * wl + x] =
                    (pyr->at(l - 1, static_cast<int>(x * 2),
                             static_cast<int>(y * 2)) +
                     pyr->at(l - 1, static_cast<int>(x * 2 + 1),
                             static_cast<int>(y * 2)) +
                     pyr->at(l - 1, static_cast<int>(x * 2),
                             static_cast<int>(y * 2 + 1)) +
                     pyr->at(l - 1, static_cast<int>(x * 2 + 1),
                             static_cast<int>(y * 2 + 1))) *
                    0.25f;
    }
}

struct Feature {
    uint32_t id = 0;
    float x = 0.0f, y = 0.0f;   // level-0 px
    bool alive = true;
};

// Harris corners on level 0, best-per-cell NMS, ordered by (response
// desc, y, x) so the pick is deterministic. Cells occupied by live
// features are skipped so re-detection fills gaps instead of doubling.
void detect_features(const Pyramid& pyr, std::vector<Feature>* feats,
                     uint32_t* next_id) {
    const uint32_t w = pyr.w[0], h = pyr.h[0];
    constexpr int kCell = 24;
    const int cw = static_cast<int>(w) / kCell + 1;
    const int ch = static_cast<int>(h) / kCell + 1;
    std::vector<uint8_t> occupied(static_cast<size_t>(cw) * ch, 0);
    int live = 0;
    for (const Feature& f : *feats)
        if (f.alive) {
            ++live;
            const int cx = static_cast<int>(f.x) / kCell;
            const int cy = static_cast<int>(f.y) / kCell;
            if (cx >= 0 && cx < cw && cy >= 0 && cy < ch)
                occupied[static_cast<size_t>(cy) * cw + cx] = 1;
        }
    if (live >= kMaxFeatures) return;

    struct Corner {
        float score, x, y;
        int cell;
    };
    std::vector<Corner> best(static_cast<size_t>(cw) * ch,
                             {0.0f, 0.0f, 0.0f, -1});
    for (int y = 4; y < static_cast<int>(h) - 4; ++y)
        for (int x = 4; x < static_cast<int>(w) - 4; ++x) {
            const int cell = (y / kCell) * cw + (x / kCell);
            if (occupied[static_cast<size_t>(cell)]) continue;
            float ixx = 0.0f, iyy = 0.0f, ixy = 0.0f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const float gx = (pyr.at(0, x + dx + 1, y + dy) -
                                      pyr.at(0, x + dx - 1, y + dy)) *
                                     0.5f;
                    const float gy = (pyr.at(0, x + dx, y + dy + 1) -
                                      pyr.at(0, x + dx, y + dy - 1)) *
                                     0.5f;
                    ixx += gx * gx;
                    iyy += gy * gy;
                    ixy += gx * gy;
                }
            const float det = ixx * iyy - ixy * ixy;
            const float tr = ixx + iyy;
            const float score = det - 0.06f * tr * tr;
            if (score > best[static_cast<size_t>(cell)].score)
                best[static_cast<size_t>(cell)] = {
                    score, static_cast<float>(x), static_cast<float>(y),
                    cell};
        }
    std::vector<Corner> picks;
    for (const Corner& c : best)
        if (c.cell >= 0 && c.score > 500.0f) picks.push_back(c);
    std::stable_sort(picks.begin(), picks.end(),
                     [](const Corner& a, const Corner& b) {
                         if (a.score != b.score) return a.score > b.score;
                         if (a.y != b.y) return a.y < b.y;
                         return a.x < b.x;
                     });
    for (const Corner& c : picks) {
        if (live >= kMaxFeatures) break;
        Feature f;
        f.id = (*next_id)++;
        f.x = c.x;
        f.y = c.y;
        feats->push_back(f);
        ++live;
    }
}

// One feature through the pyramid, prev -> cur. Returns false on a lost
// track (out of bounds, flat patch, high residual).
bool klt_track(const Pyramid& prev, const Pyramid& cur, float* io_x,
               float* io_y) {
    const float sub = static_cast<float>(1 << (kPyrLevels - 1));
    float gx_total = 0.0f, gy_total = 0.0f;   // guess, top-level px
    float px = *io_x / sub, py = *io_y / sub;
    for (int l = kPyrLevels - 1; l >= 0; --l) {
        // Spatial gradient matrix over the window at the PREV position.
        float ixx = 0.0f, iyy = 0.0f, ixy = 0.0f;
        float grads_x[(2 * kKltWin + 1) * (2 * kKltWin + 1)];
        float grads_y[(2 * kKltWin + 1) * (2 * kKltWin + 1)];
        float vals[(2 * kKltWin + 1) * (2 * kKltWin + 1)];
        int gi = 0;
        for (int dy = -kKltWin; dy <= kKltWin; ++dy)
            for (int dx = -kKltWin; dx <= kKltWin; ++dx, ++gi) {
                const float sx = px + static_cast<float>(dx);
                const float sy = py + static_cast<float>(dy);
                const float gx =
                    (prev.bilinear(l, sx + 1.0f, sy) -
                     prev.bilinear(l, sx - 1.0f, sy)) * 0.5f;
                const float gy =
                    (prev.bilinear(l, sx, sy + 1.0f) -
                     prev.bilinear(l, sx, sy - 1.0f)) * 0.5f;
                grads_x[gi] = gx;
                grads_y[gi] = gy;
                vals[gi] = prev.bilinear(l, sx, sy);
                ixx += gx * gx;
                iyy += gy * gy;
                ixy += gx * gy;
            }
        const float det = ixx * iyy - ixy * ixy;
        const float mineig =
            0.5f * (ixx + iyy -
                    std::sqrt((ixx - iyy) * (ixx - iyy) +
                              4.0f * ixy * ixy));
        if (l == 0 && mineig < kMinEigen) return false;
        if (std::fabs(det) < 1.0e-6f) {
            if (l == 0) return false;
            px = px * 2.0f;
            py = py * 2.0f;
            gx_total *= 2.0f;
            gy_total *= 2.0f;
            continue;
        }
        for (int it = 0; it < kKltIters; ++it) {
            float bx = 0.0f, by = 0.0f;
            gi = 0;
            for (int dy = -kKltWin; dy <= kKltWin; ++dy)
                for (int dx = -kKltWin; dx <= kKltWin; ++dx, ++gi) {
                    const float diff =
                        cur.bilinear(l, px + gx_total +
                                            static_cast<float>(dx),
                                     py + gy_total +
                                         static_cast<float>(dy)) -
                        vals[gi];
                    bx += diff * grads_x[gi];
                    by += diff * grads_y[gi];
                }
            const float ux = (iyy * -bx - -ixy * by) / det;
            const float uy = (ixx * -by - -ixy * bx) / det;
            gx_total += ux;
            gy_total += uy;
            if (ux * ux + uy * uy < 0.0004f) break;
        }
        if (l > 0) {
            px = px * 2.0f;
            py = py * 2.0f;
            gx_total *= 2.0f;
            gy_total *= 2.0f;
        }
    }
    const float nx = px + gx_total, ny = py + gy_total;
    if (nx < 4.0f || ny < 4.0f ||
        nx >= static_cast<float>(cur.w[0]) - 4.0f ||
        ny >= static_cast<float>(cur.h[0]) - 4.0f)
        return false;
    // Residual: mean abs diff over the window at the solution.
    float resid = 0.0f;
    int n = 0;
    for (int dy = -kKltWin; dy <= kKltWin; ++dy)
        for (int dx = -kKltWin; dx <= kKltWin; ++dx, ++n)
            resid += std::fabs(
                cur.bilinear(0, nx + static_cast<float>(dx),
                             ny + static_cast<float>(dy)) -
                prev.bilinear(0, px + static_cast<float>(dx),
                              py + static_cast<float>(dy)));
    if (resid / static_cast<float>(n) > kMaxResidual) return false;
    *io_x = nx;
    *io_y = ny;
    return true;
}

struct Sim {
    float tx = 0.0f, ty = 0.0f, rot = 0.0f, scale = 1.0f;
};

Sim compose(const Sim& outer, const Sim& inner) {
    // outer(inner(p)): p -> s_i*R_i*p + t_i -> s_o*R_o*(that) + t_o.
    Sim r;
    r.scale = outer.scale * inner.scale;
    r.rot = outer.rot + inner.rot;
    const float c = std::cos(outer.rot), s = std::sin(outer.rot);
    r.tx = outer.scale * (c * inner.tx - s * inner.ty) + outer.tx;
    r.ty = outer.scale * (s * inner.tx + c * inner.ty) + outer.ty;
    return r;
}

// Closed-form least-squares similarity a -> b over paired points
// (metric coords). False when degenerate.
bool fit_similarity(const std::vector<float>& ax, const std::vector<float>& ay,
                    const std::vector<float>& bx, const std::vector<float>& by,
                    const std::vector<int>& idx, Sim* out) {
    const size_t n = idx.size();
    if (n < 2) return false;
    float max_ = 0.0f, may_ = 0.0f, mbx = 0.0f, mby = 0.0f;
    for (int i : idx) {
        max_ += ax[static_cast<size_t>(i)];
        may_ += ay[static_cast<size_t>(i)];
        mbx += bx[static_cast<size_t>(i)];
        mby += by[static_cast<size_t>(i)];
    }
    const float inv = 1.0f / static_cast<float>(n);
    max_ *= inv;
    may_ *= inv;
    mbx *= inv;
    mby *= inv;
    float sxx = 0.0f, sxy = 0.0f, saa = 0.0f;
    for (int i : idx) {
        const float axc = ax[static_cast<size_t>(i)] - max_;
        const float ayc = ay[static_cast<size_t>(i)] - may_;
        const float bxc = bx[static_cast<size_t>(i)] - mbx;
        const float byc = by[static_cast<size_t>(i)] - mby;
        sxx += axc * bxc + ayc * byc;
        sxy += axc * byc - ayc * bxc;
        saa += axc * axc + ayc * ayc;
    }
    if (saa < 1.0e-10f) return false;
    out->rot = std::atan2(sxy, sxx);
    out->scale = std::sqrt(sxx * sxx + sxy * sxy) / saa;
    out->scale = std::clamp(out->scale, 0.5f, 2.0f);
    const float c = std::cos(out->rot) * out->scale;
    const float s = std::sin(out->rot) * out->scale;
    out->tx = mbx - (c * max_ - s * may_);
    out->ty = mby - (s * max_ + c * may_);
    return true;
}

}  // namespace

bool track_run(uint32_t start, uint32_t end,
               const std::function<bool(uint32_t, GrayFrame*)>& next,
               TrackData* out, const std::vector<uint32_t>& cuts,
               const std::function<void(uint32_t)>& progress,
               const std::function<bool()>& cancelled) {
    if (end <= start) return false;
    out->start = start;
    out->end = end;
    out->solve.clear();
    out->tracks.clear();
    out->mean_error = 0.0f;
    out->cuts.clear();
    out->sfm.clear();
    for (uint32_t c : cuts)
        if (c > start && c < end) out->cuts.push_back(c);
    std::sort(out->cuts.begin(), out->cuts.end());
    out->cuts.erase(std::unique(out->cuts.begin(), out->cuts.end()),
                    out->cuts.end());

    Pyramid pyr_a, pyr_b;
    Pyramid* prev = &pyr_a;
    Pyramid* cur = &pyr_b;
    std::vector<Feature> feats;
    uint32_t next_id = 1;
    std::vector<FeatureTrack> done;
    std::vector<FeatureTrack> open;   // parallel to feats by id lookup

    auto record_point = [&](const Feature& f, uint32_t frame, float w,
                            float h) {
        for (FeatureTrack& t : open)
            if (t.id == f.id) {
                t.points.push_back({frame, f.x / w, f.y / h});
                return;
            }
        FeatureTrack t;
        t.id = f.id;
        t.points.push_back({frame, f.x / w, f.y / h});
        open.push_back(std::move(t));
    };
    auto retire = [&](uint32_t fid) {
        for (size_t i = 0; i < open.size(); ++i)
            if (open[i].id == fid) {
                if (open[i].points.size() >= 2)
                    done.push_back(std::move(open[i]));
                open.erase(open.begin() + static_cast<ptrdiff_t>(i));
                return;
            }
    };

    Sim total;   // start -> current frame, metric units
    float err_sum = 0.0f;
    uint32_t err_n = 0;

    for (uint32_t frame = start; frame < end; ++frame) {
        if (cancelled && cancelled()) return false;
        GrayFrame gf;
        if (!next(frame, &gf) || !gf.data || !gf.width || !gf.height)
            return false;
        build_pyramid(gf, cur);
        const float w0 = static_cast<float>(cur->w[0]);
        const float h0 = static_cast<float>(cur->h[0]);
        const float aspect = w0 / h0;
        if (frame == start) out->aspect = aspect;

        SolveFrame sf;
        const bool at_cut =
            frame != start &&
            std::binary_search(out->cuts.begin(), out->cuts.end(), frame);
        if (at_cut) {
            // New shot: nothing tracks across the boundary. Drop every
            // live feature, re-anchor the chain at identity, detect
            // fresh.
            for (Feature& f : feats)
                if (f.alive) {
                    f.alive = false;
                    retire(f.id);
                }
            feats.clear();
            total = Sim{};
            detect_features(*cur, &feats, &next_id);
            for (const Feature& f : feats)
                record_point(f, frame, w0, h0);
        } else if (frame == start) {
            detect_features(*cur, &feats, &next_id);
            for (const Feature& f : feats)
                record_point(f, frame, w0, h0);
        } else {
            // Track every live feature prev -> cur, fixed order.
            std::vector<float> pax, pay, pbx, pby;   // metric pairs
            for (Feature& f : feats) {
                if (!f.alive) continue;
                const float ox = f.x, oy = f.y;
                if (klt_track(*prev, *cur, &f.x, &f.y)) {
                    pax.push_back(ox / h0);   // prev, metric
                    pay.push_back(oy / h0);
                    pbx.push_back(f.x / h0);  // cur, metric
                    pby.push_back(f.y / h0);
                    record_point(f, frame, w0, h0);
                } else {
                    f.alive = false;
                    retire(f.id);
                }
            }
            // Frame-to-frame similarity, RANSAC over the pairs, then a
            // least-squares refit on the inlier set.
            Sim delta;
            std::vector<int> best_in;
            const size_t np = pax.size();
            if (np >= 2) {
                for (int it = 0; it < kRansacIters; ++it) {
                    const uint32_t r0 = static_cast<uint32_t>(
                        hash_combine(hash_combine(0x7C4Full, frame),
                                     static_cast<uint64_t>(it * 2)) %
                        np);
                    const uint32_t r1 = static_cast<uint32_t>(
                        hash_combine(hash_combine(0x7C4Full, frame),
                                     static_cast<uint64_t>(it * 2 + 1)) %
                        np);
                    if (r0 == r1) continue;
                    std::vector<int> seed = {static_cast<int>(r0),
                                             static_cast<int>(r1)};
                    Sim cand;
                    if (!fit_similarity(pax, pay, pbx, pby, seed, &cand))
                        continue;
                    std::vector<int> inl;
                    const float cc = std::cos(cand.rot) * cand.scale;
                    const float ss = std::sin(cand.rot) * cand.scale;
                    for (size_t i = 0; i < np; ++i) {
                        const float px2 =
                            cc * pax[i] - ss * pay[i] + cand.tx;
                        const float py2 =
                            ss * pax[i] + cc * pay[i] + cand.ty;
                        const float dx = px2 - pbx[i];
                        const float dy = py2 - pby[i];
                        if (dx * dx + dy * dy <
                            kInlierThresh * kInlierThresh)
                            inl.push_back(static_cast<int>(i));
                    }
                    if (inl.size() > best_in.size()) best_in = inl;
                }
                if (best_in.size() >= 2)
                    fit_similarity(pax, pay, pbx, pby, best_in, &delta);
            }
            total = compose(delta, total);
            // Residual on the inliers under the refit.
            if (!best_in.empty()) {
                const float cc = std::cos(delta.rot) * delta.scale;
                const float ss = std::sin(delta.rot) * delta.scale;
                float e = 0.0f;
                for (int i : best_in) {
                    const float px2 = cc * pax[static_cast<size_t>(i)] -
                                      ss * pay[static_cast<size_t>(i)] +
                                      delta.tx;
                    const float py2 = ss * pax[static_cast<size_t>(i)] +
                                      cc * pay[static_cast<size_t>(i)] +
                                      delta.ty;
                    const float dx = px2 - pbx[static_cast<size_t>(i)];
                    const float dy = py2 - pby[static_cast<size_t>(i)];
                    e += std::sqrt(dx * dx + dy * dy);
                }
                sf.error = e / static_cast<float>(best_in.size());
                sf.inliers = static_cast<uint32_t>(best_in.size());
                err_sum += sf.error;
                ++err_n;
            }
            // Re-detect on the fixed cadence or when thin.
            int live = 0;
            for (const Feature& f : feats)
                if (f.alive) ++live;
            if (live < kRedetectBelow ||
                (frame - start) % kRedetectEvery == 0) {
                const uint32_t first_new = next_id;
                detect_features(*cur, &feats, &next_id);
                for (const Feature& f : feats)
                    if (f.alive && f.id >= first_new)
                        record_point(f, frame, w0, h0);
            }
            // Compact retired features so the scan stays bounded.
            feats.erase(std::remove_if(feats.begin(), feats.end(),
                                       [](const Feature& f) {
                                           return !f.alive;
                                       }),
                        feats.end());
        }
        sf.tx = total.tx / aspect;   // metric x back to width fractions
        sf.ty = total.ty;
        sf.rot = total.rot;
        sf.scale = total.scale;
        out->solve.push_back(sf);
        std::swap(prev, cur);
        if (progress) progress(frame - start + 1);
    }
    for (FeatureTrack& t : open)
        if (t.points.size() >= 2) done.push_back(std::move(t));
    std::stable_sort(done.begin(), done.end(),
                     [](const FeatureTrack& a, const FeatureTrack& b) {
                         return a.id < b.id;
                     });
    out->tracks = std::move(done);
    out->mean_error = err_n ? err_sum / static_cast<float>(err_n) : 0.0f;
    return true;
}

namespace {

// 3x3 helpers, row-major.
void h_mul(const float* a, const float* b, float* out) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] +
                             a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
}

void h_apply(const float* h, float x, float y, float* ox, float* oy) {
    const float w = h[6] * x + h[7] * y + h[8];
    const float iw = std::fabs(w) > 1.0e-12f ? 1.0f / w : 0.0f;
    *ox = (h[0] * x + h[1] * y + h[2]) * iw;
    *oy = (h[3] * x + h[4] * y + h[5]) * iw;
}

// Unit square -> quad homography (the corner-pin adjugate construction).
bool h_from_quad(const float qx[4], const float qy[4], float* out) {
    // Corners ordered 00, 10, 01, 11.
    const float dx1 = qx[1] - qx[3], dy1 = qy[1] - qy[3];
    const float dx2 = qx[2] - qx[3], dy2 = qy[2] - qy[3];
    const float sx = qx[0] - qx[1] + qx[3] - qx[2];
    const float sy = qy[0] - qy[1] + qy[3] - qy[2];
    const float det = dx1 * dy2 - dy1 * dx2;
    if (std::fabs(det) < 1.0e-12f) return false;
    const float g = (sx * dy2 - sy * dx2) / det;
    const float h8 = (dx1 * sy - dy1 * sx) / det;
    out[0] = qx[1] - qx[0] + g * qx[1];
    out[1] = qx[2] - qx[0] + h8 * qx[2];
    out[2] = qx[0];
    out[3] = qy[1] - qy[0] + g * qy[1];
    out[4] = qy[2] - qy[0] + h8 * qy[2];
    out[5] = qy[0];
    out[6] = g;
    out[7] = h8;
    out[8] = 1.0f;
    return true;
}

bool h_invert(const float* h, float* out) {
    const float a = h[0], b = h[1], c = h[2];
    const float d = h[3], e = h[4], f = h[5];
    const float g = h[6], i = h[7], j = h[8];
    out[0] = e * j - f * i;
    out[1] = c * i - b * j;
    out[2] = b * f - c * e;
    out[3] = f * g - d * j;
    out[4] = a * j - c * g;
    out[5] = c * d - a * f;
    out[6] = d * i - e * g;
    out[7] = b * g - a * i;
    out[8] = a * e - b * d;
    const float det = a * out[0] + b * out[3] + c * out[6];
    if (std::fabs(det) < 1.0e-12f) return false;
    const float inv = 1.0f / det;
    for (int k = 0; k < 9; ++k) out[k] *= inv;
    return true;
}

// Exact 4-point homography a -> b: square->b composed with inverse of
// square->a.
bool h_4point(const float ax[4], const float ay[4], const float bx[4],
              const float by[4], float* out) {
    float ha[9], hb[9], hai[9];
    if (!h_from_quad(ax, ay, ha) || !h_from_quad(bx, by, hb)) return false;
    if (!h_invert(ha, hai)) return false;
    h_mul(hb, hai, out);
    return true;
}

// Least-squares refit with h33 = 1 (normal equations, 8x8 Gaussian
// elimination). Good in the planar-track regime; falls back to the
// seed when the system degenerates.
bool h_refit(const std::vector<float>& ax, const std::vector<float>& ay,
             const std::vector<float>& bx, const std::vector<float>& by,
             const std::vector<int>& idx, float* io_h) {
    if (idx.size() < 4) return false;
    double ata[8][8] = {};
    double atb[8] = {};
    for (int ii : idx) {
        const size_t i = static_cast<size_t>(ii);
        const double x = ax[i], y = ay[i], u = bx[i], v = by[i];
        // Row 1: [x y 1 0 0 0 -ux -uy] . h = u
        // Row 2: [0 0 0 x y 1 -vx -vy] . h = v
        const double r1[8] = {x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y};
        const double r2[8] = {0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y};
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c)
                ata[r][c] += r1[r] * r1[c] + r2[r] * r2[c];
            atb[r] += r1[r] * u + r2[r] * v;
        }
    }
    // Gaussian elimination with partial pivoting, fixed order.
    int perm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int col = 0; col < 8; ++col) {
        int piv = col;
        for (int r = col + 1; r < 8; ++r)
            if (std::fabs(ata[r][col]) > std::fabs(ata[piv][col])) piv = r;
        if (std::fabs(ata[piv][col]) < 1.0e-12) return false;
        if (piv != col) {
            for (int c = 0; c < 8; ++c) std::swap(ata[piv][c], ata[col][c]);
            std::swap(atb[piv], atb[col]);
            std::swap(perm[piv], perm[col]);
        }
        for (int r = col + 1; r < 8; ++r) {
            const double f = ata[r][col] / ata[col][col];
            for (int c = col; c < 8; ++c) ata[r][c] -= f * ata[col][c];
            atb[r] -= f * atb[col];
        }
    }
    double x[8];
    for (int r = 7; r >= 0; --r) {
        double s = atb[r];
        for (int c = r + 1; c < 8; ++c) s -= ata[r][c] * x[c];
        x[r] = s / ata[r][r];
    }
    (void)perm;
    for (int k = 0; k < 8; ++k) io_h[k] = static_cast<float>(x[k]);
    io_h[8] = 1.0f;
    return true;
}

// ------------------------------------------------------------- 3D solve
//
// Per-shot structure from motion over the stored tracks. Coordinates:
// observations are METRIC (centered uv, y down, x scaled by aspect so a
// height is 1); cameras are world-to-camera x_cam = R(aa)·X + t with z
// forward; projection is f·(x/z, y/z) in the same metric units, so
// every residual below reads in canvas heights. The world frame is the
// bootstrap camera; the bootstrap baseline is unit length (the gauge).

using util::jacobi_eigen_sym;
using util::m3_det;
using util::m3_invert;
using util::m3_mul;
using util::ransac_pick;
using util::rodrigues;
using util::rodrigues_inv;
using util::rodrigues_jac;
using util::svd3;
using util::sym_solve;

constexpr uint32_t kSfmMinFrames = 10;
constexpr uint32_t kSfmBaWindow = 400;    // bundle-adjusted cams cap; the
                                          // tail localizes against the
                                          // adjusted structure
constexpr int kSfmMinShared = 16;         // bootstrap pair floor
constexpr double kSfmMinMotion = 0.006;   // median disparity, heights
constexpr int kSfmEssIters = 192;
constexpr double kSfmReprojGate = 0.012;  // triangulation accept, heights
constexpr double kSfmGrossGate = 0.06;    // adjustment intake gate
constexpr int kSfmBaIters = 24;           // LM cap per round
constexpr int kSfmBaRounds = 2;           // adjust -> prune -> adjust
constexpr int kSfmCamIters = 12;          // camera-only LM cap
constexpr int kSfmMinPoints = 16;
constexpr double kSfmMaxReproj = 0.02;    // solved-status ceiling
constexpr double kSfmHomographyShare = 0.92;

// Metric observation of a track at an absolute frame (tracks store
// consecutive frames, so the lookup is index arithmetic).
bool track_obs(const FeatureTrack& t, uint32_t f, double aspect,
               double* mx, double* my) {
    if (t.points.empty() || f < t.points.front().frame ||
        f > t.points.back().frame)
        return false;
    const TrackPoint& p = t.points[f - t.points.front().frame];
    if (p.frame != f) return false;
    *mx = (static_cast<double>(p.x) - 0.5) * aspect;
    *my = static_cast<double>(p.y) - 0.5;
    return true;
}

void cam_apply(const double r[9], const double t[3], const double X[3],
               double out[3]) {
    out[0] = r[0] * X[0] + r[1] * X[1] + r[2] * X[2] + t[0];
    out[1] = r[3] * X[0] + r[4] * X[1] + r[5] * X[2] + t[1];
    out[2] = r[6] * X[0] + r[7] * X[1] + r[8] * X[2] + t[2];
}

// Squared metric reprojection error; a point behind the camera reads
// as a fixed fat residual so gates and error sums stay finite.
double reproj_sq(const double r[9], const double t[3], double f,
                 const double X[3], double mx, double my) {
    double c[3];
    cam_apply(r, t, X, c);
    if (c[2] < 1.0e-9) return 1.0;
    const double du = f * c[0] / c[2] - mx;
    const double dv = f * c[1] / c[2] - my;
    return du * du + dv * dv;
}

// Least-squares essential matrix over the index set (normalized
// coordinates): nullspace eigenvector of A^T A, spectrum forced to
// (1, 1, 0) through the SVD.
void essential_fit(const std::vector<double>& ax,
                   const std::vector<double>& ay,
                   const std::vector<double>& bx,
                   const std::vector<double>& by, const int* idx, int n,
                   double e_out[9]) {
    double ata[81] = {};
    for (int k = 0; k < n; ++k) {
        const size_t i = static_cast<size_t>(idx[k]);
        const double x[3] = {ax[i], ay[i], 1.0};
        const double xp[3] = {bx[i], by[i], 1.0};
        double row[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) row[r * 3 + c] = xp[r] * x[c];
        for (int r = 0; r < 9; ++r)
            for (int c = 0; c < 9; ++c) ata[r * 9 + c] += row[r] * row[c];
    }
    double evals[9], evecs[81];
    jacobi_eigen_sym(ata, 9, evals, evecs);
    double e[9];
    for (int k = 0; k < 9; ++k) e[k] = evecs[k];   // smallest eigenvalue
    double u[9], s[3], vt[9];
    svd3(e, u, s, vt);
    const double d110[9] = {1, 0, 0, 0, 1, 0, 0, 0, 0};
    double us[9];
    m3_mul(u, d110, us);
    m3_mul(us, vt, e_out);
}

double sampson_sq(const double e[9], double x1, double y1, double x2,
                  double y2) {
    const double ex0 = e[0] * x1 + e[1] * y1 + e[2];
    const double ex1 = e[3] * x1 + e[4] * y1 + e[5];
    const double ex2 = e[6] * x1 + e[7] * y1 + e[8];
    const double et0 = e[0] * x2 + e[3] * y2 + e[6];
    const double et1 = e[1] * x2 + e[4] * y2 + e[7];
    const double xex = x2 * ex0 + y2 * ex1 + ex2;
    const double den = ex0 * ex0 + ex1 * ex1 + et0 * et0 + et1 * et1;
    return den > 1.0e-18 ? xex * xex / den : 1.0e18;
}

// Two-view DLT triangulation with P = [R|t] on normalized coordinates:
// nullspace of the 4x4 A^T A.
bool triangulate2(const double r1[9], const double t1[3], double x1,
                  double y1, const double r2[9], const double t2[3],
                  double x2, double y2, double X_out[3]) {
    double A[16];
    auto fill = [&](int base, const double r[9], const double t[3],
                    double x, double y) {
        for (int c = 0; c < 3; ++c) {
            A[base * 4 + c] = x * r[6 + c] - r[c];
            A[(base + 1) * 4 + c] = y * r[6 + c] - r[3 + c];
        }
        A[base * 4 + 3] = x * t[2] - t[0];
        A[(base + 1) * 4 + 3] = y * t[2] - t[1];
    };
    fill(0, r1, t1, x1, y1);
    fill(2, r2, t2, x2, y2);
    double ata[16] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                ata[r * 4 + c] += A[k * 4 + r] * A[k * 4 + c];
    double ev[4], evec[16];
    jacobi_eigen_sym(ata, 4, ev, evec);
    const double w = evec[3];
    if (std::fabs(w) < 1.0e-12) return false;
    X_out[0] = evec[0] / w;
    X_out[1] = evec[1] / w;
    X_out[2] = evec[2] / w;
    return true;
}

// Damped Gauss-Newton refine of one camera against fixed points.
// Fixed iteration cap; the damping schedule reacts to the error but
// depends only on the values, so the path is deterministic.
void refine_camera(const std::vector<double>& X,
                   const std::vector<double>& obs, double f, double aa[3],
                   double t[3]) {
    const size_t n = X.size() / 3;
    if (n < 3) return;
    auto total_err = [&](const double* a3, const double* t3) {
        double r[9];
        rodrigues(a3, r);
        double e = 0.0;
        for (size_t i = 0; i < n; ++i)
            e += reproj_sq(r, t3, f, &X[i * 3], obs[i * 2], obs[i * 2 + 1]);
        return e;
    };
    double err = total_err(aa, t);
    double lambda = 1.0e-3;
    for (int it = 0; it < kSfmCamIters; ++it) {
        double r[9], jr[27];
        rodrigues_jac(aa, r, jr);
        double H[36] = {}, g[6] = {};
        for (size_t i = 0; i < n; ++i) {
            double c[3];
            cam_apply(r, t, &X[i * 3], c);
            if (c[2] < 1.0e-9) continue;
            const double iz = 1.0 / c[2];
            const double ru = f * c[0] * iz - obs[i * 2];
            const double rv = f * c[1] * iz - obs[i * 2 + 1];
            double J[2][6];
            for (int k = 0; k < 6; ++k) {
                double dc[3];
                if (k < 3) {
                    for (int d = 0; d < 3; ++d)
                        dc[d] = jr[(d * 3 + 0) * 3 + k] * X[i * 3] +
                                jr[(d * 3 + 1) * 3 + k] * X[i * 3 + 1] +
                                jr[(d * 3 + 2) * 3 + k] * X[i * 3 + 2];
                } else {
                    dc[0] = k == 3 ? 1.0 : 0.0;
                    dc[1] = k == 4 ? 1.0 : 0.0;
                    dc[2] = k == 5 ? 1.0 : 0.0;
                }
                J[0][k] = f * iz * (dc[0] - c[0] * iz * dc[2]);
                J[1][k] = f * iz * (dc[1] - c[1] * iz * dc[2]);
            }
            for (int a2 = 0; a2 < 6; ++a2) {
                for (int b2 = 0; b2 < 6; ++b2)
                    H[a2 * 6 + b2] +=
                        J[0][a2] * J[0][b2] + J[1][a2] * J[1][b2];
                g[a2] -= J[0][a2] * ru + J[1][a2] * rv;
            }
        }
        double Hd[36];
        std::memcpy(Hd, H, sizeof(H));
        for (int d = 0; d < 6; ++d) Hd[d * 6 + d] *= 1.0 + lambda;
        double step[6];
        std::memcpy(step, g, sizeof(g));
        if (!sym_solve(Hd, 6, step)) {
            lambda = std::min(lambda * 8.0, 1.0e8);
            continue;
        }
        const double aa2[3] = {aa[0] + step[0], aa[1] + step[1],
                               aa[2] + step[2]};
        const double t2[3] = {t[0] + step[3], t[1] + step[4],
                              t[2] + step[5]};
        const double e2 = total_err(aa2, t2);
        if (e2 < err) {
            std::memcpy(aa, aa2, sizeof(aa2));
            std::memcpy(t, t2, sizeof(t2));
            err = e2;
            lambda = std::max(lambda * 0.25, 1.0e-9);
        } else {
            lambda = std::min(lambda * 8.0, 1.0e8);
        }
    }
}

// Homography inlier count over the pair's metric correspondences (the
// existing 4-point RANSAC + LS refit) - the degeneracy arbiter: motion
// a plane explains as well as the essential is a pan/static/planar
// case that must NOT pretend to 3D-solve.
int homography_inliers(const std::vector<float>& pax,
                       const std::vector<float>& pay,
                       const std::vector<float>& pbx,
                       const std::vector<float>& pby, uint64_t seed) {
    const size_t np = pax.size();
    if (np < 4) return 0;
    int best = 0;
    for (int it = 0; it < kSfmEssIters; ++it) {
        int pick[4];
        if (!ransac_pick(seed, static_cast<uint32_t>(it),
                         static_cast<uint32_t>(np), 4, pick))
            break;
        float ax4[4], ay4[4], bx4[4], by4[4];
        for (int s = 0; s < 4; ++s) {
            ax4[s] = pax[static_cast<size_t>(pick[s])];
            ay4[s] = pay[static_cast<size_t>(pick[s])];
            bx4[s] = pbx[static_cast<size_t>(pick[s])];
            by4[s] = pby[static_cast<size_t>(pick[s])];
        }
        float cand[9];
        if (!h_4point(ax4, ay4, bx4, by4, cand)) continue;
        int inl = 0;
        for (size_t i = 0; i < np; ++i) {
            float px2, py2;
            h_apply(cand, pax[i], pay[i], &px2, &py2);
            const float dx = px2 - pbx[i], dy = py2 - pby[i];
            if (dx * dx + dy * dy < kInlierThresh * kInlierThresh) ++inl;
        }
        if (inl > best) best = inl;
    }
    return best;
}

// One shot's full solve. The pipeline: keyframe pair -> essential ->
// cheirality -> triangulate -> incremental resection -> bundle
// adjustment (two rounds around an observation prune). Long shots
// adjust the first kSfmBaWindow cameras and localize the tail against
// that structure, so poses stay in one gauge at bounded memory.
void solve_segment(const TrackData& data, uint32_t s, uint32_t e,
                   SfmSegment* seg) {
    seg->start = s;
    seg->end = e;
    seg->status = kSfmUnsolved;
    seg->focal = 1.0;
    seg->mean_reproj = 0.0f;
    seg->cams.clear();
    seg->points.clear();
    const double aspect = static_cast<double>(data.aspect);
    const uint32_t nf = e - s;
    if (nf < kSfmMinFrames) {
        seg->status = kSfmTooShort;
        return;
    }
    const uint32_t ba_nf = std::min(nf, kSfmBaWindow);

    // Tracks with at least two observations inside the segment, id
    // order (tracks never span cuts, so clipping is interval math).
    struct SegTrack {
        uint32_t t_index = 0;
        uint32_t first = 0, last = 0;   // inclusive absolute frames
        int point = -1;                 // index into pts once triangulated
    };
    std::vector<SegTrack> st;
    for (uint32_t ti = 0; ti < data.tracks.size(); ++ti) {
        const FeatureTrack& t = data.tracks[ti];
        if (t.points.empty()) continue;
        const uint32_t f0 = std::max(t.points.front().frame, s);
        const uint32_t f1 = std::min(t.points.back().frame, e - 1);
        if (f0 >= f1) continue;
        st.push_back({ti, f0, f1, -1});
    }
    if (st.size() < static_cast<size_t>(kSfmMinShared)) return;

    // ---- keyframe pair: anchors every 6 frames across the whole
    // window (real footage can open on a fade or junk cohort - the
    // trackable stretch lives wherever it lives), partner scan, score
    // = shared count x capped median disparity. Static footage never
    // clears the motion floor and reports low-parallax honestly.
    uint32_t best_i = 0, best_j = 0;
    double best_score = 0.0;
    bool saw_pair = false;
    std::vector<double> disp;
    for (uint32_t i = s; i + 3 < s + ba_nf; i += 6) {
        const uint32_t j_end = std::min(s + ba_nf, i + 121);
        for (uint32_t j = i + 3; j < j_end; ++j) {
            int shared = 0;
            disp.clear();
            for (const SegTrack& tk : st) {
                if (tk.first > i || tk.last < j) continue;
                double ax_, ay_, bx_, by_;
                if (!track_obs(data.tracks[tk.t_index], i, aspect, &ax_,
                               &ay_) ||
                    !track_obs(data.tracks[tk.t_index], j, aspect, &bx_,
                               &by_))
                    continue;
                ++shared;
                const double dx = bx_ - ax_, dy = by_ - ay_;
                disp.push_back(std::sqrt(dx * dx + dy * dy));
            }
            if (shared < kSfmMinShared) continue;
            saw_pair = true;
            std::nth_element(disp.begin(),
                             disp.begin() +
                                 static_cast<ptrdiff_t>(disp.size() / 2),
                             disp.end());
            const double med = disp[disp.size() / 2];
            if (med < kSfmMinMotion) continue;
            const double score =
                static_cast<double>(shared) * std::min(med, 0.12);
            if (score > best_score) {
                best_score = score;
                best_i = i;
                best_j = j;
            }
        }
    }
    if (best_score <= 0.0) {
        if (saw_pair) seg->status = kSfmLowParallax;
        return;
    }

    // Pair correspondences, metric.
    std::vector<double> pax, pay, pbx, pby;
    std::vector<uint32_t> pair_st;
    for (uint32_t k = 0; k < st.size(); ++k) {
        const SegTrack& tk = st[k];
        if (tk.first > best_i || tk.last < best_j) continue;
        double ax_, ay_, bx_, by_;
        if (!track_obs(data.tracks[tk.t_index], best_i, aspect, &ax_,
                       &ay_) ||
            !track_obs(data.tracks[tk.t_index], best_j, aspect, &bx_,
                       &by_))
            continue;
        pax.push_back(ax_);
        pay.push_back(ay_);
        pbx.push_back(bx_);
        pby.push_back(by_);
        pair_st.push_back(k);
    }
    const size_t np = pax.size();
    if (np < 8) return;

    // ---- focal sweep: essential RANSAC per candidate, scored by the
    // cheirality front count (then inliers). The winner seeds the
    // adjustment, which refines focal continuously.
    const double kFocals[6] = {0.6, 0.8, 1.0, 1.3, 1.7, 2.4};
    double focal = 0.0;
    double boot_r[9], boot_t[3];
    std::vector<int> boot_inl;
    int best_front = -1;
    size_t best_ninl = 0;
    const uint64_t ess_seed = hash_combine(0xE55Eull, s);
    for (double fc : kFocals) {
        std::vector<double> nax(np), nay(np), nbx(np), nby(np);
        for (size_t k = 0; k < np; ++k) {
            nax[k] = pax[k] / fc;
            nay[k] = pay[k] / fc;
            nbx[k] = pbx[k] / fc;
            nby[k] = pby[k] / fc;
        }
        const double thr = kInlierThresh / fc;
        const double thr2 = thr * thr;
        std::vector<int> binl;
        double be[9] = {};
        for (int it = 0; it < kSfmEssIters; ++it) {
            int pick[8];
            if (!ransac_pick(ess_seed, static_cast<uint32_t>(it),
                             static_cast<uint32_t>(np), 8, pick))
                break;
            double e9[9];
            essential_fit(nax, nay, nbx, nby, pick, 8, e9);
            std::vector<int> inl;
            for (size_t k = 0; k < np; ++k)
                if (sampson_sq(e9, nax[k], nay[k], nbx[k], nby[k]) < thr2)
                    inl.push_back(static_cast<int>(k));
            if (inl.size() > binl.size()) {
                binl = std::move(inl);
                std::memcpy(be, e9, sizeof(e9));
            }
        }
        if (binl.size() < 8) continue;
        // LS refit on the consensus, then recount.
        double e9[9];
        essential_fit(nax, nay, nbx, nby, binl.data(),
                      static_cast<int>(binl.size()), e9);
        std::vector<int> inl2;
        for (size_t k = 0; k < np; ++k)
            if (sampson_sq(e9, nax[k], nay[k], nbx[k], nby[k]) < thr2)
                inl2.push_back(static_cast<int>(k));
        if (inl2.size() >= binl.size()) {
            binl = std::move(inl2);
            std::memcpy(be, e9, sizeof(e9));
        }
        // Decompose; four (R, t) candidates; cheirality votes.
        double u[9], sv[3], vt[9];
        svd3(be, u, sv, vt);
        if (m3_det(u) < 0.0) {   // proper U: flip the null column
            u[2] = -u[2];
            u[5] = -u[5];
            u[8] = -u[8];
        }
        const double w9[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
        const double wt9[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
        const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const double zero3[3] = {0, 0, 0};
        for (int rc = 0; rc < 2; ++rc) {
            double tmp[9], R9[9];
            m3_mul(u, rc == 0 ? w9 : wt9, tmp);
            m3_mul(tmp, vt, R9);
            if (m3_det(R9) < 0.0)
                for (int k = 0; k < 9; ++k) R9[k] = -R9[k];
            for (int ts = 0; ts < 2; ++ts) {
                const double sgn = ts == 0 ? 1.0 : -1.0;
                const double t3[3] = {sgn * u[2], sgn * u[5],
                                      sgn * u[8]};
                int front = 0;
                for (int k : binl) {
                    const size_t ki = static_cast<size_t>(k);
                    double X[3];
                    if (!triangulate2(eye, zero3, nax[ki], nay[ki], R9,
                                      t3, nbx[ki], nby[ki], X))
                        continue;
                    if (X[2] <= 1.0e-6) continue;
                    double c2[3];
                    cam_apply(R9, t3, X, c2);
                    if (c2[2] <= 1.0e-6) continue;
                    ++front;
                }
                if (front > best_front ||
                    (front == best_front && binl.size() > best_ninl)) {
                    best_front = front;
                    best_ninl = binl.size();
                    focal = fc;
                    std::memcpy(boot_r, R9, sizeof(R9));
                    std::memcpy(boot_t, t3, sizeof(t3));
                    boot_inl = binl;
                }
            }
        }
    }
    if (best_front < kSfmMinShared / 2) return;

    // ---- degeneracy arbiter: a homography that explains the essential
    // consensus as well is a pan / static / planar shot.
    {
        std::vector<float> fax(np), fay(np), fbx(np), fby(np);
        for (size_t k = 0; k < np; ++k) {
            fax[k] = static_cast<float>(pax[k]);
            fay[k] = static_cast<float>(pay[k]);
            fbx[k] = static_cast<float>(pbx[k]);
            fby[k] = static_cast<float>(pby[k]);
        }
        const int h_inl =
            homography_inliers(fax, fay, fbx, fby,
                               hash_combine(0x40D0ull, s));
        if (static_cast<double>(h_inl) >=
            kSfmHomographyShare * static_cast<double>(boot_inl.size())) {
            seg->status = kSfmLowParallax;
            return;
        }
    }

    // ---- bootstrap: cameras at the pair, points from its inliers.
    const uint32_t li = best_i - s, lj = best_j - s;
    std::vector<SfmCamera> cams(nf);
    rodrigues_inv(boot_r, cams[lj].aa);
    std::memcpy(cams[lj].t, boot_t, sizeof(boot_t));
    std::vector<double> pts;          // 3 per point
    std::vector<uint32_t> pt_track;   // FeatureTrack id per point
    std::vector<uint8_t> pt_active;
    const double eye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double zero3[3] = {0, 0, 0};
    auto try_point = [&](uint32_t sti, const double r1[9],
                         const double t1[3], double u1, double v1,
                         const double r2[9], const double t2[3],
                         double u2, double v2) {
        double X[3];
        if (!triangulate2(r1, t1, u1 / focal, v1 / focal, r2, t2,
                          u2 / focal, v2 / focal, X))
            return;
        double c1[3], c2[3];
        cam_apply(r1, t1, X, c1);
        cam_apply(r2, t2, X, c2);
        if (c1[2] <= 1.0e-6 || c2[2] <= 1.0e-6) return;
        if (reproj_sq(r1, t1, focal, X, u1, v1) >
                kSfmReprojGate * kSfmReprojGate ||
            reproj_sq(r2, t2, focal, X, u2, v2) >
                kSfmReprojGate * kSfmReprojGate)
            return;
        st[sti].point = static_cast<int>(pt_track.size());
        pts.push_back(X[0]);
        pts.push_back(X[1]);
        pts.push_back(X[2]);
        pt_track.push_back(data.tracks[st[sti].t_index].id);
        pt_active.push_back(1);
    };
    for (int k : boot_inl) {
        const size_t ki = static_cast<size_t>(k);
        try_point(pair_st[ki], eye, zero3, pax[ki], pay[ki], boot_r,
                  boot_t, pbx[ki], pby[ki]);
    }
    if (pt_track.size() < static_cast<size_t>(kSfmMinPoints)) return;

    // ---- incremental resection + triangulation. Order: between the
    // pair (left neighbor init), forward past it, then backward - all
    // fixed. Each solved frame may triangulate tracks it newly covers.
    std::vector<uint8_t> cam_solved(nf, 0);
    cam_solved[li] = cam_solved[lj] = 1;
    std::vector<double> rs_cache(nf * 9, 0.0);
    rodrigues(cams[li].aa, &rs_cache[li * 9]);
    rodrigues(cams[lj].aa, &rs_cache[lj * 9]);
    auto resect = [&](uint32_t lf, uint32_t init_from) {
        cams[lf] = cams[init_from];
        std::vector<double> X, ob;
        double r0[9];
        rodrigues(cams[lf].aa, r0);
        for (const SegTrack& tk : st) {
            if (tk.point < 0 || !pt_active[static_cast<size_t>(tk.point)])
                continue;
            double mx, my;
            if (!track_obs(data.tracks[tk.t_index], s + lf, aspect, &mx,
                           &my))
                continue;
            const double* P = &pts[static_cast<size_t>(tk.point) * 3];
            // Loose gate against the init pose keeps gross outliers out
            // of the refine.
            if (reproj_sq(r0, cams[lf].t, focal, P, mx, my) > 0.08 * 0.08)
                continue;
            X.insert(X.end(), {P[0], P[1], P[2]});
            ob.insert(ob.end(), {mx, my});
        }
        if (X.size() / 3 >= 6)
            refine_camera(X, ob, focal, cams[lf].aa, cams[lf].t);
        cam_solved[lf] = 1;
        rodrigues(cams[lf].aa, &rs_cache[lf * 9]);
        // New tracks first seen well at this frame: partner = solved
        // frame with the widest disparity among the track's span.
        for (uint32_t sti = 0; sti < st.size(); ++sti) {
            SegTrack& tk = st[sti];
            if (tk.point >= 0) continue;
            double gu, gv;
            if (!track_obs(data.tracks[tk.t_index], s + lf, aspect, &gu,
                           &gv))
                continue;
            uint32_t bestf = 0;
            double bestd = 0.0;
            bool havef = false;
            for (uint32_t f2 = tk.first; f2 <= tk.last; ++f2) {
                const uint32_t l2 = f2 - s;
                if (l2 >= nf || !cam_solved[l2] || l2 == lf) continue;
                double u2, v2;
                if (!track_obs(data.tracks[tk.t_index], f2, aspect, &u2,
                               &v2))
                    continue;
                const double dx = u2 - gu, dy = v2 - gv;
                const double d = std::sqrt(dx * dx + dy * dy);
                if (!havef || d > bestd) {
                    havef = true;
                    bestd = d;
                    bestf = l2;
                }
            }
            if (!havef || bestd < kSfmMinMotion) continue;
            double u2, v2;
            track_obs(data.tracks[tk.t_index], s + bestf, aspect, &u2,
                      &v2);
            try_point(sti, &rs_cache[bestf * 9], cams[bestf].t, u2, v2,
                      &rs_cache[lf * 9], cams[lf].t, gu, gv);
        }
    };
    for (uint32_t lf = li + 1; lf < lj; ++lf) resect(lf, lf - 1);
    for (uint32_t lf = lj + 1; lf < ba_nf; ++lf) resect(lf, lf - 1);
    for (uint32_t lf = li; lf-- > 0;) resect(lf, lf + 1);

    // ---- bundle adjustment over the window: free cameras (all but
    // the gauge) + shared focal on the reduced side, point blocks
    // Schur-eliminated. Fixed iteration cap; value-driven damping and
    // early convergence exit keep the path deterministic.
    struct BaObs {
        uint32_t cam = 0, pt = 0;
        double u = 0.0, v = 0.0;
        uint8_t active = 1;
    };
    std::vector<BaObs> obs;
    for (const SegTrack& tk : st) {
        if (tk.point < 0) continue;
        const uint32_t f_end = std::min(tk.last, s + ba_nf - 1);
        for (uint32_t f = tk.first; f <= f_end; ++f) {
            double mx, my;
            if (!track_obs(data.tracks[tk.t_index], f, aspect, &mx, &my))
                continue;
            obs.push_back(
                {f - s, static_cast<uint32_t>(tk.point), mx, my, 1});
        }
    }
    std::stable_sort(obs.begin(), obs.end(),
                     [](const BaObs& a, const BaObs& b) {
                         if (a.pt != b.pt) return a.pt < b.pt;
                         return a.cam < b.cam;
                     });
    for (BaObs& o : obs)
        if (reproj_sq(&rs_cache[o.cam * 9], cams[o.cam].t, focal,
                      &pts[o.pt * 3], o.u, o.v) >
            kSfmGrossGate * kSfmGrossGate)
            o.active = 0;

    const size_t npts = pt_track.size();
    std::vector<int> col(ba_nf, -1);
    int nfree = 0;
    for (uint32_t c2 = 0; c2 < ba_nf; ++c2)
        if (c2 != li) col[c2] = 6 * nfree++;
    const int fcol = 6 * nfree;
    const int m = fcol + 1;

    auto full_err = [&](const std::vector<SfmCamera>& cs,
                        const std::vector<double>& ps, double fc) {
        std::vector<double> rr(ba_nf * 9);
        for (uint32_t c2 = 0; c2 < ba_nf; ++c2)
            rodrigues(cs[c2].aa, &rr[c2 * 9]);
        double e2 = 0.0;
        for (const BaObs& o : obs)
            if (o.active && pt_active[o.pt])
                e2 += reproj_sq(&rr[o.cam * 9], cs[o.cam].t, fc,
                                &ps[o.pt * 3], o.u, o.v);
        return e2;
    };

    auto lm_round = [&]() {
        const size_t no = obs.size();
        std::vector<double> Jc(no * 12), Jp(no * 6), res(no * 2);
        std::vector<uint8_t> ok(no);
        std::vector<double> S(static_cast<size_t>(m) * m), rhs(m);
        std::vector<double> V(npts * 9), gp(npts * 3), Yp(npts * 3),
            Vi(npts * 9);
        std::vector<uint8_t> vi_ok(npts);
        std::vector<double> rsj(ba_nf * 9), jrs(ba_nf * 27);
        std::vector<double> Wbuf, Abuf;
        std::vector<size_t> use;
        double lambda = 1.0e-3;
        double err = full_err(cams, pts, focal);
        for (int it = 0; it < kSfmBaIters; ++it) {
            for (uint32_t c2 = 0; c2 < ba_nf; ++c2)
                rodrigues_jac(cams[c2].aa, &rsj[c2 * 9], &jrs[c2 * 27]);
            std::fill(S.begin(), S.end(), 0.0);
            std::fill(rhs.begin(), rhs.end(), 0.0);
            std::fill(V.begin(), V.end(), 0.0);
            std::fill(gp.begin(), gp.end(), 0.0);
            std::fill(Yp.begin(), Yp.end(), 0.0);
            std::fill(ok.begin(), ok.end(), 0);
            for (size_t oi = 0; oi < no; ++oi) {
                const BaObs& o = obs[oi];
                if (!o.active || !pt_active[o.pt]) continue;
                const double* r9 = &rsj[o.cam * 9];
                const double* jr = &jrs[o.cam * 27];
                const double* X = &pts[o.pt * 3];
                double c3[3];
                cam_apply(r9, cams[o.cam].t, X, c3);
                if (c3[2] < 1.0e-9) continue;
                ok[oi] = 1;
                const double iz = 1.0 / c3[2];
                const double ru = focal * c3[0] * iz - o.u;
                const double rv = focal * c3[1] * iz - o.v;
                res[oi * 2] = ru;
                res[oi * 2 + 1] = rv;
                double* jc = &Jc[oi * 12];
                for (int k = 0; k < 6; ++k) {
                    double dc[3];
                    if (k < 3) {
                        for (int d = 0; d < 3; ++d)
                            dc[d] = jr[(d * 3 + 0) * 3 + k] * X[0] +
                                    jr[(d * 3 + 1) * 3 + k] * X[1] +
                                    jr[(d * 3 + 2) * 3 + k] * X[2];
                    } else {
                        dc[0] = k == 3 ? 1.0 : 0.0;
                        dc[1] = k == 4 ? 1.0 : 0.0;
                        dc[2] = k == 5 ? 1.0 : 0.0;
                    }
                    jc[k] = focal * iz * (dc[0] - c3[0] * iz * dc[2]);
                    jc[6 + k] =
                        focal * iz * (dc[1] - c3[1] * iz * dc[2]);
                }
                double* jp = &Jp[oi * 6];
                for (int k = 0; k < 3; ++k) {
                    jp[k] =
                        focal * iz * (r9[k] - c3[0] * iz * r9[6 + k]);
                    jp[3 + k] = focal * iz *
                                (r9[3 + k] - c3[1] * iz * r9[6 + k]);
                }
                const double jf0 = c3[0] * iz, jf1 = c3[1] * iz;
                const int cc = col[o.cam];
                if (cc >= 0) {
                    for (int a2 = 0; a2 < 6; ++a2) {
                        double* srow =
                            &S[static_cast<size_t>(cc + a2) * m];
                        for (int b2 = 0; b2 < 6; ++b2)
                            srow[cc + b2] += jc[a2] * jc[b2] +
                                             jc[6 + a2] * jc[6 + b2];
                        const double cf =
                            jc[a2] * jf0 + jc[6 + a2] * jf1;
                        srow[fcol] += cf;
                        S[static_cast<size_t>(fcol) * m + cc + a2] += cf;
                        rhs[cc + a2] -= jc[a2] * ru + jc[6 + a2] * rv;
                    }
                }
                S[static_cast<size_t>(fcol) * m + fcol] +=
                    jf0 * jf0 + jf1 * jf1;
                rhs[fcol] -= jf0 * ru + jf1 * rv;
                double* vp = &V[static_cast<size_t>(o.pt) * 9];
                double* gpp = &gp[static_cast<size_t>(o.pt) * 3];
                double* yp = &Yp[static_cast<size_t>(o.pt) * 3];
                for (int a2 = 0; a2 < 3; ++a2) {
                    for (int b2 = 0; b2 < 3; ++b2)
                        vp[a2 * 3 + b2] +=
                            jp[a2] * jp[b2] + jp[3 + a2] * jp[3 + b2];
                    gpp[a2] -= jp[a2] * ru + jp[3 + a2] * rv;
                    yp[a2] += jp[a2] * jf0 + jp[3 + a2] * jf1;
                }
            }
            // Marquardt damping plus an additive floor: a camera with
            // no live observations has an all-zero block that no
            // multiplicative damping can lift - the floor makes its
            // step exactly zero (gradient is zero too), holding the
            // resection pose instead of sinking the whole solve.
            double dmax = 0.0;
            for (int d = 0; d < m; ++d)
                dmax = std::max(dmax,
                                S[static_cast<size_t>(d) * m + d]);
            const double dfloor =
                std::max(1.0e-12, 1.0e-10 * dmax) * (1.0 + lambda);
            for (int d = 0; d < m; ++d)
                S[static_cast<size_t>(d) * m + d] =
                    S[static_cast<size_t>(d) * m + d] * (1.0 + lambda) +
                    dfloor;
            for (size_t p = 0; p < npts; ++p)
                for (int d = 0; d < 3; ++d)
                    V[p * 9 + d * 3 + d] *= 1.0 + lambda;
            std::fill(vi_ok.begin(), vi_ok.end(), 0);
            size_t o0 = 0;
            while (o0 < obs.size()) {
                size_t o1 = o0;
                const uint32_t p = obs[o0].pt;
                while (o1 < obs.size() && obs[o1].pt == p) ++o1;
                if (!pt_active[p]) {
                    o0 = o1;
                    continue;
                }
                double vinv[9];
                if (!m3_invert(&V[static_cast<size_t>(p) * 9], vinv)) {
                    o0 = o1;
                    continue;
                }
                vi_ok[p] = 1;
                std::memcpy(&Vi[static_cast<size_t>(p) * 9], vinv,
                            sizeof(vinv));
                double y3[3], z3[3];
                util::m3_mul_v(vinv, &gp[static_cast<size_t>(p) * 3], y3);
                util::m3_mul_v(vinv, &Yp[static_cast<size_t>(p) * 3], z3);
                use.clear();
                for (size_t i = o0; i < o1; ++i)
                    if (ok[i]) use.push_back(i);
                Wbuf.resize(use.size() * 18);
                Abuf.resize(use.size() * 18);
                for (size_t ui = 0; ui < use.size(); ++ui) {
                    const double* jc = &Jc[use[ui] * 12];
                    const double* jp = &Jp[use[ui] * 6];
                    double* w = &Wbuf[ui * 18];
                    for (int a2 = 0; a2 < 6; ++a2)
                        for (int b2 = 0; b2 < 3; ++b2)
                            w[a2 * 3 + b2] = jc[a2] * jp[b2] +
                                             jc[6 + a2] * jp[3 + b2];
                    double* aw = &Abuf[ui * 18];
                    for (int a2 = 0; a2 < 6; ++a2)
                        for (int b2 = 0; b2 < 3; ++b2)
                            aw[a2 * 3 + b2] = w[a2 * 3] * vinv[b2] +
                                              w[a2 * 3 + 1] *
                                                  vinv[3 + b2] +
                                              w[a2 * 3 + 2] *
                                                  vinv[6 + b2];
                }
                const double* ypp = &Yp[static_cast<size_t>(p) * 3];
                for (size_t ui = 0; ui < use.size(); ++ui) {
                    const int ci = col[obs[use[ui]].cam];
                    if (ci < 0) continue;
                    const double* w = &Wbuf[ui * 18];
                    for (int a2 = 0; a2 < 6; ++a2) {
                        rhs[ci + a2] -= w[a2 * 3] * y3[0] +
                                        w[a2 * 3 + 1] * y3[1] +
                                        w[a2 * 3 + 2] * y3[2];
                        const double fz = w[a2 * 3] * z3[0] +
                                          w[a2 * 3 + 1] * z3[1] +
                                          w[a2 * 3 + 2] * z3[2];
                        S[static_cast<size_t>(fcol) * m + ci + a2] -= fz;
                        S[static_cast<size_t>(ci + a2) * m + fcol] -= fz;
                    }
                    const double* ai = &Abuf[ui * 18];
                    for (size_t uj = 0; uj < use.size(); ++uj) {
                        const int cj = col[obs[use[uj]].cam];
                        if (cj < 0) continue;
                        const double* wj = &Wbuf[uj * 18];
                        for (int a2 = 0; a2 < 6; ++a2) {
                            double* srow =
                                &S[static_cast<size_t>(ci + a2) * m];
                            for (int b2 = 0; b2 < 6; ++b2)
                                srow[cj + b2] -=
                                    ai[a2 * 3] * wj[b2 * 3] +
                                    ai[a2 * 3 + 1] * wj[b2 * 3 + 1] +
                                    ai[a2 * 3 + 2] * wj[b2 * 3 + 2];
                        }
                    }
                }
                S[static_cast<size_t>(fcol) * m + fcol] -=
                    ypp[0] * z3[0] + ypp[1] * z3[1] + ypp[2] * z3[2];
                rhs[fcol] -= ypp[0] * y3[0] + ypp[1] * y3[1] +
                             ypp[2] * y3[2];
                o0 = o1;
            }
            if (!sym_solve(S.data(), m, rhs.data())) {
                // More damping makes S diagonally dominant; the
                // iteration cap bounds the hunt.
                lambda = std::min(lambda * 8.0, 1.0e8);
                continue;
            }
            const double df = rhs[fcol];
            std::vector<SfmCamera> cams2 = cams;
            for (uint32_t c2 = 0; c2 < ba_nf; ++c2) {
                const int cc = col[c2];
                if (cc < 0) continue;
                for (int k = 0; k < 3; ++k) {
                    cams2[c2].aa[k] += rhs[cc + k];
                    cams2[c2].t[k] += rhs[cc + 3 + k];
                }
            }
            const double focal2 =
                std::min(6.0, std::max(0.3, focal + df));
            std::vector<double> pts2 = pts;
            o0 = 0;
            while (o0 < obs.size()) {
                size_t o1 = o0;
                const uint32_t p = obs[o0].pt;
                while (o1 < obs.size() && obs[o1].pt == p) ++o1;
                if (!vi_ok[p]) {
                    o0 = o1;
                    continue;
                }
                double acc[3] = {gp[static_cast<size_t>(p) * 3],
                                 gp[static_cast<size_t>(p) * 3 + 1],
                                 gp[static_cast<size_t>(p) * 3 + 2]};
                for (size_t i = o0; i < o1; ++i) {
                    if (!ok[i]) continue;
                    const int ci = col[obs[i].cam];
                    if (ci < 0) continue;
                    const double* jc = &Jc[i * 12];
                    const double* jp = &Jp[i * 6];
                    for (int b2 = 0; b2 < 3; ++b2) {
                        double wtd = 0.0;
                        for (int a2 = 0; a2 < 6; ++a2)
                            wtd += (jc[a2] * jp[b2] +
                                    jc[6 + a2] * jp[3 + b2]) *
                                   rhs[ci + a2];
                        acc[b2] -= wtd;
                    }
                }
                for (int b2 = 0; b2 < 3; ++b2)
                    acc[b2] -= Yp[static_cast<size_t>(p) * 3 + b2] * df;
                double dp[3];
                util::m3_mul_v(&Vi[static_cast<size_t>(p) * 9], acc, dp);
                pts2[static_cast<size_t>(p) * 3] += dp[0];
                pts2[static_cast<size_t>(p) * 3 + 1] += dp[1];
                pts2[static_cast<size_t>(p) * 3 + 2] += dp[2];
                o0 = o1;
            }
            const double err2 = full_err(cams2, pts2, focal2);
            if (err2 < err) {
                const double gain = err - err2;
                cams = std::move(cams2);
                pts = std::move(pts2);
                focal = focal2;
                err = err2;
                lambda = std::max(lambda * 0.25, 1.0e-9);
                if (gain < 1.0e-10 * (err + 1.0e-12)) break;
            } else {
                lambda = std::min(lambda * 8.0, 1.0e8);
            }
        }
    };

    auto prune = [&]() {
        std::vector<double> rr(ba_nf * 9);
        for (uint32_t c2 = 0; c2 < ba_nf; ++c2)
            rodrigues(cams[c2].aa, &rr[c2 * 9]);
        std::vector<double> ee(obs.size(), 0.0);
        double sum = 0.0;
        size_t cnt = 0;
        for (size_t oi = 0; oi < obs.size(); ++oi) {
            const BaObs& o = obs[oi];
            if (!o.active || !pt_active[o.pt]) continue;
            ee[oi] = std::sqrt(reproj_sq(&rr[o.cam * 9], cams[o.cam].t,
                                         focal, &pts[o.pt * 3], o.u,
                                         o.v));
            sum += ee[oi];
            ++cnt;
        }
        if (!cnt) return;
        const double gate = std::max(2.5 * sum / static_cast<double>(cnt),
                                     0.006);
        for (size_t oi = 0; oi < obs.size(); ++oi)
            if (obs[oi].active && pt_active[obs[oi].pt] &&
                ee[oi] > gate)
                obs[oi].active = 0;
        std::vector<uint32_t> per_pt(npts, 0);
        for (const BaObs& o : obs)
            if (o.active) ++per_pt[o.pt];
        for (size_t p = 0; p < npts; ++p)
            if (per_pt[p] < 2) pt_active[p] = 0;
    };

    for (int rd = 0; rd < kSfmBaRounds; ++rd) {
        lm_round();
        if (rd + 1 < kSfmBaRounds) prune();
    }

    // Tail beyond the adjustment window: localize against the adjusted
    // structure, triangulating fresh points as the view moves on.
    for (uint32_t c2 = 0; c2 < ba_nf; ++c2)
        rodrigues(cams[c2].aa, &rs_cache[c2 * 9]);
    for (uint32_t lf = ba_nf; lf < nf; ++lf) resect(lf, lf - 1);

    // Final stats over the adjusted window.
    double mean = 0.0;
    size_t cnt = 0;
    {
        std::vector<double> rr(ba_nf * 9);
        for (uint32_t c2 = 0; c2 < ba_nf; ++c2)
            rodrigues(cams[c2].aa, &rr[c2 * 9]);
        for (const BaObs& o : obs) {
            if (!o.active || !pt_active[o.pt]) continue;
            mean += std::sqrt(reproj_sq(&rr[o.cam * 9], cams[o.cam].t,
                                        focal, &pts[o.pt * 3], o.u,
                                        o.v));
            ++cnt;
        }
    }
    mean = cnt ? mean / static_cast<double>(cnt) : 1.0;
    size_t nact = 0;
    for (size_t p = 0; p < pt_track.size(); ++p)
        if (pt_active[p]) ++nact;
    if (nact < static_cast<size_t>(kSfmMinPoints) ||
        mean > kSfmMaxReproj)
        return;   // stays kSfmUnsolved, cams/points withheld
    seg->status = kSfmSolved;
    seg->focal = focal;
    seg->mean_reproj = static_cast<float>(mean);
    seg->cams.resize(nf);
    for (uint32_t lf = 0; lf < nf; ++lf) seg->cams[lf] = cams[lf];
    for (size_t p = 0; p < pt_track.size(); ++p)
        if (pt_active[p])
            seg->points.push_back({pt_track[p], pts[p * 3],
                                   pts[p * 3 + 1], pts[p * 3 + 2]});
}

// 3D plane path for ensure_plane: when the range-start shot solved,
// fit a plane to the solved points the region covers at the anchor
// frame and induce each frame's homography from the camera poses -
// perspective-true where the chained 2D fits only approximate. Frames
// past the shot freeze at the last in-shot value.
bool plane_from_sfm(const TrackData& data, float rx, float ry, float rw,
                    float rh, PlaneSolve* out) {
    if (data.sfm.empty()) return false;
    const SfmSegment& seg = data.sfm.front();
    if (seg.status != kSfmSolved || seg.start != data.start) return false;
    const uint32_t n = data.end - data.start;
    const uint32_t sn = seg.end - seg.start;
    if (seg.cams.size() < sn || sn == 0) return false;
    const double aspect = static_cast<double>(data.aspect);
    const double f = seg.focal;
    double ra[9];
    rodrigues(seg.cams[0].aa, ra);
    const double* ta = seg.cams[0].t;
    // Region membership at the anchor frame, anchor-camera coords.
    std::vector<double> P;
    for (const SfmPoint& sp : seg.points) {
        const double X[3] = {sp.x, sp.y, sp.z};
        double c[3];
        cam_apply(ra, ta, X, c);
        if (c[2] < 1.0e-6) continue;
        const double u = (f * c[0] / c[2]) / aspect + 0.5;
        const double v = f * c[1] / c[2] + 0.5;
        if (std::fabs(u - rx) > rw * 0.5 || std::fabs(v - ry) > rh * 0.5)
            continue;
        P.push_back(c[0]);
        P.push_back(c[1]);
        P.push_back(c[2]);
    }
    const size_t npt = P.size() / 3;
    if (npt < 8) return false;
    // Plane fit: centroid + smallest covariance eigenvector; one
    // reweight pass sheds points off the dominant surface.
    std::vector<uint8_t> keep(npt, 1);
    double nrm[3] = {0, 0, 1}, dd = 0.0;
    auto fit = [&]() -> bool {
        double c0[3] = {};
        size_t cnt = 0;
        for (size_t i = 0; i < npt; ++i) {
            if (!keep[i]) continue;
            c0[0] += P[i * 3];
            c0[1] += P[i * 3 + 1];
            c0[2] += P[i * 3 + 2];
            ++cnt;
        }
        if (cnt < 3) return false;
        for (double& v : c0) v /= static_cast<double>(cnt);
        double cov[9] = {};
        for (size_t i = 0; i < npt; ++i) {
            if (!keep[i]) continue;
            const double dx[3] = {P[i * 3] - c0[0], P[i * 3 + 1] - c0[1],
                                  P[i * 3 + 2] - c0[2]};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) cov[r * 3 + c] += dx[r] * dx[c];
        }
        double ev[3], evec[9];
        jacobi_eigen_sym(cov, 3, ev, evec);
        nrm[0] = evec[0];
        nrm[1] = evec[1];
        nrm[2] = evec[2];
        dd = nrm[0] * c0[0] + nrm[1] * c0[1] + nrm[2] * c0[2];
        return true;
    };
    if (!fit()) return false;
    std::vector<double> dev(npt);
    for (size_t i = 0; i < npt; ++i)
        dev[i] = std::fabs(nrm[0] * P[i * 3] + nrm[1] * P[i * 3 + 1] +
                           nrm[2] * P[i * 3 + 2] - dd);
    std::vector<double> med = dev;
    std::nth_element(med.begin(),
                     med.begin() + static_cast<ptrdiff_t>(med.size() / 2),
                     med.end());
    const double gate = std::max(3.0 * med[med.size() / 2], 1.0e-4);
    for (size_t i = 0; i < npt; ++i) keep[i] = dev[i] <= gate ? 1 : 0;
    if (!fit()) return false;
    if (std::fabs(dd) < 1.0e-9) return false;
    // uv -> metric and the (shared-focal) calibration, both invertible
    // by construction.
    const double M[9] = {aspect, 0, -0.5 * aspect, 0, 1, -0.5, 0, 0, 1};
    double Minv[9];
    if (!m3_invert(M, Minv)) return false;
    const double K[9] = {f, 0, 0, 0, f, 0, 0, 0, 1};
    const double Kinv[9] = {1.0 / f, 0, 0, 0, 1.0 / f, 0, 0, 0, 1};
    out->rx = rx;
    out->ry = ry;
    out->rw = rw;
    out->rh = rh;
    out->h.assign(static_cast<size_t>(n) * 9, 0.0f);
    float last[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double rat[9];
    util::m3_transpose(ra, rat);
    for (uint32_t k = 0; k < n; ++k) {
        if (k < sn) {
            double rg[9];
            rodrigues(seg.cams[k].aa, rg);
            double rrel[9];
            m3_mul(rg, rat, rrel);
            double tt[3];
            util::m3_mul_v(rrel, ta, tt);
            const double trel[3] = {seg.cams[k].t[0] - tt[0],
                                    seg.cams[k].t[1] - tt[1],
                                    seg.cams[k].t[2] - tt[2]};
            double hn[9];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    hn[r * 3 + c] = rrel[r * 3 + c] -
                                    trel[r] * nrm[c] / dd;
            double t1[9], t2[9];
            m3_mul(K, hn, t1);
            m3_mul(t1, Kinv, t2);
            m3_mul(Minv, t2, t1);
            m3_mul(t1, M, t2);
            if (std::fabs(t2[8]) < 1.0e-12) return false;
            for (int q = 0; q < 9; ++q)
                last[q] = static_cast<float>(t2[q] / t2[8]);
        }
        std::memcpy(out->h.data() + static_cast<size_t>(k) * 9, last,
                    9 * sizeof(float));
    }
    return true;
}

}  // namespace

bool sfm_solve(TrackData* data) {
    if (!data) return false;
    data->sfm.clear();
    if (data->end <= data->start) return false;
    std::vector<uint32_t> bounds;
    bounds.push_back(data->start);
    for (uint32_t c : data->cuts) bounds.push_back(c);
    bounds.push_back(data->end);
    bool any = false;
    for (size_t k = 0; k + 1 < bounds.size(); ++k) {
        SfmSegment seg;
        solve_segment(*data, bounds[k], bounds[k + 1], &seg);
        if (seg.status == kSfmSolved) any = true;
        data->sfm.push_back(std::move(seg));
    }
    return any;
}

const PlaneSolve* ensure_plane(TrackData* data, float rx, float ry,
                               float rw, float rh) {
    constexpr float kEps = 1.0e-3f;
    for (const PlaneSolve& p : data->planes)
        if (std::fabs(p.rx - rx) < kEps && std::fabs(p.ry - ry) < kEps &&
            std::fabs(p.rw - rw) < kEps && std::fabs(p.rh - rh) < kEps)
            return &p;
    const uint32_t n = data->end - data->start;
    if (n == 0) return nullptr;
    // 3D-first: a solved range-start shot induces perspective-true
    // homographies; the chained 2D fits below are the fallback.
    {
        PlaneSolve p3;
        if (plane_from_sfm(*data, rx, ry, rw, rh, &p3)) {
            data->planes.push_back(std::move(p3));
            return &data->planes.back();
        }
    }
    // For each consecutive pair (f-1, f): tracks observed in both, whose
    // f-1 position lies inside the region AS TRACKED so far (the seed
    // rect pushed through the accumulated homography). The chain stops
    // at the first scene cut - the region's shot is over, and the new
    // shot's content must not re-capture the frozen rect.
    uint32_t freeze_k = n;
    for (uint32_t c : data->cuts)
        if (c > data->start) {
            freeze_k = c - data->start;
            break;
        }
    PlaneSolve plane;
    plane.rx = rx;
    plane.ry = ry;
    plane.rw = rw;
    plane.rh = rh;
    plane.h.resize(static_cast<size_t>(n) * 9);
    float acc[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(plane.h.data(), acc, 9 * sizeof(float));
    // Region corners at the reference frame.
    const float cx0[4] = {rx - rw * 0.5f, rx + rw * 0.5f, rx - rw * 0.5f,
                          rx + rw * 0.5f};
    const float cy0[4] = {ry - rh * 0.5f, ry - rh * 0.5f, ry + rh * 0.5f,
                          ry + rh * 0.5f};
    bool any = false;
    for (uint32_t k = 1; k < n; ++k) {
        if (k >= freeze_k) {
            std::memcpy(plane.h.data() + static_cast<size_t>(k) * 9, acc,
                        9 * sizeof(float));
            continue;
        }
        const uint32_t fa = data->start + k - 1;
        const uint32_t fb = data->start + k;
        // The region as of frame fa.
        float qx[4], qy[4];
        for (int c = 0; c < 4; ++c)
            h_apply(acc, cx0[c], cy0[c], &qx[c], &qy[c]);
        const float minx = std::min({qx[0], qx[1], qx[2], qx[3]});
        const float maxx = std::max({qx[0], qx[1], qx[2], qx[3]});
        const float miny = std::min({qy[0], qy[1], qy[2], qy[3]});
        const float maxy = std::max({qy[0], qy[1], qy[2], qy[3]});
        std::vector<float> pax, pay, pbx, pby;
        for (const FeatureTrack& t : data->tracks) {
            if (t.points.empty() || t.points.front().frame > fa ||
                t.points.back().frame < fb)
                continue;
            const size_t ia = fa - t.points.front().frame;
            const size_t ib = ia + 1;
            if (ib >= t.points.size()) continue;
            const TrackPoint& a = t.points[ia];
            const TrackPoint& b = t.points[ib];
            if (a.frame != fa || b.frame != fb) continue;
            if (a.x < minx || a.x > maxx || a.y < miny || a.y > maxy)
                continue;
            pax.push_back(a.x);
            pay.push_back(a.y);
            pbx.push_back(b.x);
            pby.push_back(b.y);
        }
        float delta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const size_t np = pax.size();
        if (np >= 4) {
            std::vector<int> best_in;
            float best_h[9];
            bool have = false;
            for (int it = 0; it < kRansacIters; ++it) {
                int pick[4];
                bool distinct = true;
                for (int s = 0; s < 4; ++s) {
                    pick[s] = static_cast<int>(
                        hash_combine(
                            hash_combine(0x9A7Eull, fb),
                            static_cast<uint64_t>(it * 4 + s)) %
                        np);
                    for (int s2 = 0; s2 < s; ++s2)
                        if (pick[s2] == pick[s]) distinct = false;
                }
                if (!distinct) continue;
                float ax4[4], ay4[4], bx4[4], by4[4];
                for (int s = 0; s < 4; ++s) {
                    ax4[s] = pax[static_cast<size_t>(pick[s])];
                    ay4[s] = pay[static_cast<size_t>(pick[s])];
                    bx4[s] = pbx[static_cast<size_t>(pick[s])];
                    by4[s] = pby[static_cast<size_t>(pick[s])];
                }
                float cand[9];
                if (!h_4point(ax4, ay4, bx4, by4, cand)) continue;
                std::vector<int> inl;
                for (size_t i = 0; i < np; ++i) {
                    float px2, py2;
                    h_apply(cand, pax[i], pay[i], &px2, &py2);
                    const float dx = px2 - pbx[i], dy = py2 - pby[i];
                    if (dx * dx + dy * dy <
                        kInlierThresh * kInlierThresh)
                        inl.push_back(static_cast<int>(i));
                }
                if (inl.size() > best_in.size()) {
                    best_in = inl;
                    std::memcpy(best_h, cand, sizeof(cand));
                    have = true;
                }
            }
            if (have && best_in.size() >= 4) {
                std::memcpy(delta, best_h, sizeof(best_h));
                h_refit(pax, pay, pbx, pby, best_in, delta);
                any = true;
            }
        }
        float next[9];
        h_mul(delta, acc, next);
        // Renormalize so h33 stays 1 across long chains.
        if (std::fabs(next[8]) > 1.0e-12f)
            for (int c = 0; c < 9; ++c) next[c] /= next[8];
        std::memcpy(acc, next, sizeof(acc));
        std::memcpy(plane.h.data() + static_cast<size_t>(k) * 9, acc,
                    9 * sizeof(float));
    }
    if (!any) return nullptr;
    data->planes.push_back(std::move(plane));
    return &data->planes.back();
}

bool track_save(const std::filesystem::path& path, const TrackData& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    auto w32 = [&](uint32_t v) {
        f.write(reinterpret_cast<const char*>(&v), 4);
    };
    auto w64 = [&](uint64_t v) {
        f.write(reinterpret_cast<const char*>(&v), 8);
    };
    auto wf = [&](float v) {
        f.write(reinterpret_cast<const char*>(&v), 4);
    };
    w32(kCacheMagic);
    w64(d.settings_hash);
    w32(d.start);
    w32(d.end);
    wf(d.mean_error);
    w32(static_cast<uint32_t>(d.solve.size()));
    for (const SolveFrame& s : d.solve) {
        wf(s.tx);
        wf(s.ty);
        wf(s.rot);
        wf(s.scale);
        wf(s.error);
        w32(s.inliers);
    }
    w32(static_cast<uint32_t>(d.tracks.size()));
    for (const FeatureTrack& t : d.tracks) {
        w32(t.id);
        w32(static_cast<uint32_t>(t.points.size()));
        for (const TrackPoint& p : t.points) {
            w32(p.frame);
            wf(p.x);
            wf(p.y);
        }
    }
    w32(static_cast<uint32_t>(d.planes.size()));
    for (const PlaneSolve& p : d.planes) {
        wf(p.rx);
        wf(p.ry);
        wf(p.rw);
        wf(p.rh);
        w32(static_cast<uint32_t>(p.h.size()));
        for (float v : p.h) wf(v);
    }
    auto wd = [&](double v) {
        f.write(reinterpret_cast<const char*>(&v), 8);
    };
    wf(d.aspect);
    w32(static_cast<uint32_t>(d.cuts.size()));
    for (uint32_t c : d.cuts) w32(c);
    w32(static_cast<uint32_t>(d.sfm.size()));
    for (const SfmSegment& seg : d.sfm) {
        w32(seg.start);
        w32(seg.end);
        w32(seg.status);
        wd(seg.focal);
        wf(seg.mean_reproj);
        w32(static_cast<uint32_t>(seg.cams.size()));
        for (const SfmCamera& c : seg.cams) {
            for (double v : c.aa) wd(v);
            for (double v : c.t) wd(v);
        }
        w32(static_cast<uint32_t>(seg.points.size()));
        for (const SfmPoint& p : seg.points) {
            w32(p.track);
            wd(p.x);
            wd(p.y);
            wd(p.z);
        }
    }
    return f.good();
}

bool track_load(const std::filesystem::path& path, TrackData* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    auto r32 = [&](uint32_t* v) {
        f.read(reinterpret_cast<char*>(v), 4);
        return f.good();
    };
    auto r64 = [&](uint64_t* v) {
        f.read(reinterpret_cast<char*>(v), 8);
        return f.good();
    };
    auto rf = [&](float* v) {
        f.read(reinterpret_cast<char*>(v), 4);
        return f.good();
    };
    uint32_t magic = 0;
    if (!r32(&magic) || magic != kCacheMagic) return false;
    if (!r64(&out->settings_hash) || !r32(&out->start) || !r32(&out->end) ||
        !rf(&out->mean_error))
        return false;
    uint32_t n = 0;
    if (!r32(&n) || n > 1000000u) return false;
    out->solve.resize(n);
    for (SolveFrame& s : out->solve)
        if (!rf(&s.tx) || !rf(&s.ty) || !rf(&s.rot) || !rf(&s.scale) ||
            !rf(&s.error) || !r32(&s.inliers))
            return false;
    if (!r32(&n) || n > 1000000u) return false;
    out->tracks.resize(n);
    for (FeatureTrack& t : out->tracks) {
        uint32_t pn = 0;
        if (!r32(&t.id) || !r32(&pn) || pn > 10000000u) return false;
        t.points.resize(pn);
        for (TrackPoint& p : t.points)
            if (!r32(&p.frame) || !rf(&p.x) || !rf(&p.y)) return false;
    }
    if (!r32(&n) || n > 4096u) return false;
    out->planes.resize(n);
    for (PlaneSolve& p : out->planes) {
        uint32_t hn = 0;
        if (!rf(&p.rx) || !rf(&p.ry) || !rf(&p.rw) || !rf(&p.rh) ||
            !r32(&hn) || hn > 100000000u)
            return false;
        p.h.resize(hn);
        for (float& v : p.h)
            if (!rf(&v)) return false;
    }
    auto rd = [&](double* v) {
        f.read(reinterpret_cast<char*>(v), 8);
        return f.good();
    };
    if (!rf(&out->aspect)) return false;
    if (!r32(&n) || n > 1000000u) return false;
    out->cuts.resize(n);
    for (uint32_t& c : out->cuts)
        if (!r32(&c)) return false;
    if (!r32(&n) || n > 1000000u) return false;
    out->sfm.resize(n);
    for (SfmSegment& seg : out->sfm) {
        if (!r32(&seg.start) || !r32(&seg.end) || !r32(&seg.status) ||
            !rd(&seg.focal) || !rf(&seg.mean_reproj))
            return false;
        uint32_t cn = 0;
        if (!r32(&cn) || cn > 1000000u) return false;
        seg.cams.resize(cn);
        for (SfmCamera& c : seg.cams) {
            for (double& v : c.aa)
                if (!rd(&v)) return false;
            for (double& v : c.t)
                if (!rd(&v)) return false;
        }
        uint32_t pn = 0;
        if (!r32(&pn) || pn > 10000000u) return false;
        seg.points.resize(pn);
        for (SfmPoint& p : seg.points)
            if (!r32(&p.track) || !rd(&p.x) || !rd(&p.y) || !rd(&p.z))
                return false;
    }
    return true;
}

}  // namespace looks::media
