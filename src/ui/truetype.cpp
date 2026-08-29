#include "ui/truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ui/font.h"
#include "util/file.h"

namespace looks::ui {

namespace {

// Bounds-checked big-endian reads; overrun returns 0, never out of range.
uint32_t rd8(const std::vector<uint8_t>& b, size_t off) {
    return off < b.size() ? b[off] : 0u;
}
uint32_t rd16(const std::vector<uint8_t>& b, size_t off) {
    if (off + 2 > b.size()) return 0;
    return (static_cast<uint32_t>(b[off]) << 8) | b[off + 1];
}
int32_t rd16s(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<int16_t>(rd16(b, off));
}
uint32_t rd32(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) return 0;
    return (static_cast<uint32_t>(b[off]) << 24) |
           (static_cast<uint32_t>(b[off + 1]) << 16) |
           (static_cast<uint32_t>(b[off + 2]) << 8) | b[off + 3];
}

// Segment in bitmap px, y-down.
struct Seg {
    float x0, y0, x1, y1;
};

constexpr uint32_t kMaxBitmapW = 3072;
constexpr uint32_t kMaxBitmapH = 1024;
constexpr size_t kMaxPoints = 20000;   // per glyph, runaway bound

}  // namespace

std::optional<TtfFont> TtfFont::load(const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes || bytes->size() < 12 || bytes->size() > (64u << 20))
        return std::nullopt;
    TtfFont f;
    f.bytes_ = std::move(*bytes);
    const std::vector<uint8_t>& b = f.bytes_;

    const uint32_t sfnt = rd32(b, 0);
    // 0x00010000 and 'true' are glyf; 'OTTO' is CFF, unsupported.
    if (sfnt != 0x00010000u && sfnt != 0x74727565u) return std::nullopt;

    uint32_t head = 0, maxp = 0, cmap = 0, hhea = 0, kern = 0;
    const uint32_t num_tables = rd16(b, 4);
    for (uint32_t i = 0; i < num_tables && i < 64; ++i) {
        const size_t rec = 12 + static_cast<size_t>(i) * 16;
        const uint32_t tag = rd32(b, rec);
        const uint32_t off = rd32(b, rec + 8);
        const uint32_t len = rd32(b, rec + 12);
        if (off > b.size() || len > b.size() - off) continue;
        switch (tag) {
            case 0x68656164u: head = off; break;              // head
            case 0x6D617870u: maxp = off; break;              // maxp
            case 0x636D6170u: cmap = off; break;              // cmap
            case 0x68686561u: hhea = off; break;              // hhea
            case 0x6B65726Eu: kern = off; break;              // kern
            case 0x676C7966u: f.glyf_off_ = off; f.glyf_len_ = len; break;
            case 0x6C6F6361u: f.loca_off_ = off; f.loca_len_ = len; break;
            case 0x686D7478u: f.hmtx_off_ = off; f.hmtx_len_ = len; break;
            default: break;
        }
    }
    if (!head || !maxp || !cmap || !hhea || !f.glyf_off_ || !f.loca_off_ ||
        !f.hmtx_off_)
        return std::nullopt;

    f.units_per_em_ = static_cast<uint16_t>(rd16(b, head + 18));
    if (f.units_per_em_ < 16) return std::nullopt;
    f.loca_long_ = static_cast<int16_t>(rd16s(b, head + 50)) != 0;
    f.num_glyphs_ = static_cast<uint16_t>(rd16(b, maxp + 4));
    if (f.num_glyphs_ == 0) return std::nullopt;
    f.ascent_units_ = static_cast<float>(rd16s(b, hhea + 4));
    f.descent_units_ = static_cast<float>(rd16s(b, hhea + 6));
    f.num_hmetrics_ = static_cast<uint16_t>(rd16(b, hhea + 34));
    if (f.num_hmetrics_ == 0) f.num_hmetrics_ = 1;

    const uint32_t n_sub = rd16(b, cmap + 2);
    uint32_t best4 = 0, best12 = 0;
    for (uint32_t i = 0; i < n_sub && i < 32; ++i) {
        const uint32_t sub = cmap + rd32(b, cmap + 4 + i * 8 + 4);
        const uint32_t fmt = rd16(b, sub);
        if (fmt == 12 && !best12) best12 = sub;
        if (fmt == 4 && !best4) best4 = sub;
    }
    if (best12) {
        f.cmap_sub_off_ = best12;
        f.cmap_format_ = 12;
    } else if (best4) {
        f.cmap_sub_off_ = best4;
        f.cmap_format_ = 4;
    } else {
        return std::nullopt;
    }

    // Accept only kern format 0 with horizontal coverage.
    if (kern) {
        const uint32_t n_kt = rd16(b, kern + 2);
        uint32_t sub = kern + 4;
        for (uint32_t i = 0; i < n_kt && i < 8; ++i) {
            const uint32_t len = rd16(b, sub + 2);
            const uint32_t coverage = rd16(b, sub + 4);
            if ((coverage & 1u) && (coverage >> 8) == 0) {
                f.kern_pair_count_ = rd16(b, sub + 6);
                f.kern_pairs_off_ = sub + 14;
                break;
            }
            if (len < 6) break;
            sub += len;
        }
    }
    return f;
}

uint32_t TtfFont::glyph_index(uint32_t cp) const {
    const std::vector<uint8_t>& b = bytes_;
    const uint32_t sub = cmap_sub_off_;
    if (cmap_format_ == 12) {
        uint32_t lo = 0, hi = rd32(b, sub + 12);
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            const size_t g = sub + 16 + static_cast<size_t>(mid) * 12;
            if (cp < rd32(b, g)) {
                hi = mid;
            } else if (cp > rd32(b, g + 4)) {
                lo = mid + 1;
            } else {
                return rd32(b, g + 8) + (cp - rd32(b, g));
            }
        }
        return 0;
    }
    // cmap format 4; BMP only.
    if (cp > 0xFFFF) return 0;
    const uint32_t seg2 = rd16(b, sub + 6);
    const uint32_t ends = sub + 14;
    const uint32_t starts = ends + seg2 + 2;
    const uint32_t deltas = starts + seg2;
    const uint32_t ranges = deltas + seg2;
    for (uint32_t i = 0; i < seg2; i += 2) {
        if (cp > rd16(b, ends + i)) continue;
        if (cp < rd16(b, starts + i)) return 0;
        const uint32_t range_off = rd16(b, ranges + i);
        const uint32_t delta = rd16(b, deltas + i);
        if (range_off == 0) return (cp + delta) & 0xFFFFu;
        const size_t addr = ranges + i + range_off +
                            2 * (cp - rd16(b, starts + i));
        const uint32_t g = rd16(b, addr);
        return g ? (g + delta) & 0xFFFFu : 0;
    }
    return 0;
}

float TtfFont::advance_units(uint32_t glyph) const {
    const uint32_t n = glyph < num_hmetrics_ ? glyph : num_hmetrics_ - 1u;
    return static_cast<float>(rd16(bytes_, hmtx_off_ + 4u * n));
}

float TtfFont::kern_units(uint32_t left, uint32_t right) const {
    if (!kern_pair_count_) return 0.0f;
    const uint32_t key_hi = left, key_lo = right;
    uint32_t lo = 0, hi = kern_pair_count_;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const size_t p = kern_pairs_off_ + static_cast<size_t>(mid) * 6;
        const uint32_t l = rd16(bytes_, p), r = rd16(bytes_, p + 2);
        if (l < key_hi || (l == key_hi && r < key_lo)) {
            lo = mid + 1;
        } else if (l > key_hi || r > key_lo) {
            hi = mid;
        } else {
            return static_cast<float>(rd16s(bytes_, p + 4));
        }
    }
    return 0.0f;
}

void TtfFont::append_outline(uint32_t glyph, const float xf[6],
                             float scale_px,
                             std::vector<std::vector<float>>* contours,
                             int depth) const {
    if (depth > 4 || glyph >= num_glyphs_) return;
    const std::vector<uint8_t>& b = bytes_;
    uint32_t off1, off2;
    if (loca_long_) {
        off1 = rd32(b, loca_off_ + 4u * glyph);
        off2 = rd32(b, loca_off_ + 4u * glyph + 4);
    } else {
        off1 = rd16(b, loca_off_ + 2u * glyph) * 2u;
        off2 = rd16(b, loca_off_ + 2u * glyph + 2) * 2u;
    }
    if (off2 <= off1 || off2 > glyf_len_) return;   // empty glyph
    const uint32_t g = glyf_off_ + off1;
    const int n_cont = static_cast<int>(rd16s(b, g));

    // Font units to px, y-up; the caller flips to y-down.
    auto map_x = [&](float x, float y) {
        return (xf[0] * x + xf[2] * y + xf[4]) * scale_px;
    };
    auto map_y = [&](float x, float y) {
        return (xf[1] * x + xf[3] * y + xf[5]) * scale_px;
    };

    if (n_cont < 0) {
        // Composite glyph: the child transform is F2Dot14 2x2 plus an offset.
        size_t p = g + 10;
        for (int guard = 0; guard < 16; ++guard) {
            const uint32_t flags = rd16(b, p);
            const uint32_t child = rd16(b, p + 2);
            p += 4;
            float dx = 0.0f, dy = 0.0f;
            if (flags & 0x0001u) {   // words
                if (flags & 0x0002u) {
                    dx = static_cast<float>(rd16s(b, p));
                    dy = static_cast<float>(rd16s(b, p + 2));
                }
                p += 4;
            } else {
                if (flags & 0x0002u) {
                    dx = static_cast<float>(static_cast<int8_t>(rd8(b, p)));
                    dy = static_cast<float>(
                        static_cast<int8_t>(rd8(b, p + 1)));
                }
                p += 2;
            }
            float a = 1.0f, bb = 0.0f, c = 0.0f, d = 1.0f;
            auto f2dot14 = [&](size_t at) {
                return static_cast<float>(rd16s(b, at)) / 16384.0f;
            };
            if (flags & 0x0008u) {
                a = d = f2dot14(p);
                p += 2;
            } else if (flags & 0x0040u) {
                a = f2dot14(p);
                d = f2dot14(p + 2);
                p += 4;
            } else if (flags & 0x0080u) {
                a = f2dot14(p);
                bb = f2dot14(p + 2);
                c = f2dot14(p + 4);
                d = f2dot14(p + 6);
                p += 8;
            }
            // cxf applies the child first, then the parent.
            const float cxf[6] = {
                xf[0] * a + xf[2] * bb, xf[1] * a + xf[3] * bb,
                xf[0] * c + xf[2] * d,  xf[1] * c + xf[3] * d,
                xf[0] * dx + xf[2] * dy + xf[4],
                xf[1] * dx + xf[3] * dy + xf[5]};
            append_outline(child, cxf, scale_px, contours, depth + 1);
            if (!(flags & 0x0020u)) break;
        }
        return;
    }
    if (n_cont == 0) return;

    const size_t ends_at = g + 10;
    const uint32_t n_pts_u =
        rd16(b, ends_at + 2u * static_cast<uint32_t>(n_cont) - 2) + 1;
    if (n_pts_u == 0 || n_pts_u > kMaxPoints) return;
    const size_t n_pts = n_pts_u;
    const uint32_t ins_len =
        rd16(b, ends_at + 2u * static_cast<uint32_t>(n_cont));
    size_t p = ends_at + 2u * static_cast<uint32_t>(n_cont) + 2 + ins_len;

    std::vector<uint8_t> flags(n_pts);
    for (size_t i = 0; i < n_pts;) {
        const uint8_t fl = static_cast<uint8_t>(rd8(b, p++));
        flags[i++] = fl;
        if (fl & 0x08u) {
            uint32_t rep = rd8(b, p++);
            while (rep-- && i < n_pts) flags[i++] = fl;
        }
    }
    std::vector<float> xs(n_pts), ys(n_pts);
    float acc = 0.0f;
    for (size_t i = 0; i < n_pts; ++i) {
        const uint8_t fl = flags[i];
        if (fl & 0x02u) {
            const float d = static_cast<float>(rd8(b, p++));
            acc += (fl & 0x10u) ? d : -d;
        } else if (!(fl & 0x10u)) {
            acc += static_cast<float>(rd16s(b, p));
            p += 2;
        }
        xs[i] = acc;
    }
    acc = 0.0f;
    for (size_t i = 0; i < n_pts; ++i) {
        const uint8_t fl = flags[i];
        if (fl & 0x04u) {
            const float d = static_cast<float>(rd8(b, p++));
            acc += (fl & 0x20u) ? d : -d;
        } else if (!(fl & 0x20u)) {
            acc += static_cast<float>(rd16s(b, p));
            p += 2;
        }
        ys[i] = acc;
    }

    // Quadratics; each off-curve pair implies an on-curve midpoint.
    auto flatten = [&](std::vector<float>& out, float x0, float y0,
                       float cx, float cy, float x1, float y1) {
        const float ext = std::fabs(x0 - cx) + std::fabs(y0 - cy) +
                          std::fabs(cx - x1) + std::fabs(cy - y1);
        const int n = std::clamp(
            static_cast<int>(std::ceil(std::sqrt(ext * 0.75f))), 2, 24);
        for (int s = 1; s <= n; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(n);
            const float u = 1.0f - t;
            out.push_back(u * u * x0 + 2.0f * u * t * cx + t * t * x1);
            out.push_back(u * u * y0 + 2.0f * u * t * cy + t * t * y1);
        }
    };

    size_t start = 0;
    for (int ci = 0; ci < n_cont; ++ci) {
        const size_t end = rd16(b, ends_at + 2u * static_cast<uint32_t>(ci));
        if (end < start || end >= n_pts) break;
        const size_t count = end - start + 1;
        if (count < 2) {
            start = end + 1;
            continue;
        }
        auto px = [&](size_t k) {
            const size_t i = start + (k % count);
            return map_x(xs[i], ys[i]);
        };
        auto py = [&](size_t k) {
            const size_t i = start + (k % count);
            return map_y(xs[i], ys[i]);
        };
        auto on = [&](size_t k) {
            return (flags[start + (k % count)] & 0x01u) != 0;
        };
        // An all-off-curve contour anchors on the first implied midpoint.
        size_t first = 0;
        while (first < count && !on(first)) ++first;
        std::vector<float> out;
        float sx, sy;
        if (first == count) {
            first = 0;
            sx = (px(0) + px(1)) * 0.5f;
            sy = (py(0) + py(1)) * 0.5f;
        } else {
            sx = px(first);
            sy = py(first);
        }
        out.push_back(sx);
        out.push_back(sy);
        float cur_x = sx, cur_y = sy;
        size_t k = first;
        size_t walked = 0;
        while (walked < count) {
            const size_t nk = k + 1;
            ++walked;
            if (on(nk)) {
                out.push_back(px(nk));
                out.push_back(py(nk));
                cur_x = px(nk);
                cur_y = py(nk);
            } else {
                float ex, ey;
                if (on(nk + 1)) {
                    ex = px(nk + 1);
                    ey = py(nk + 1);
                    ++walked;
                    k = nk;   // consume the end point too
                } else {
                    ex = (px(nk) + px(nk + 1)) * 0.5f;
                    ey = (py(nk) + py(nk + 1)) * 0.5f;
                }
                flatten(out, cur_x, cur_y, px(nk), py(nk), ex, ey);
                cur_x = ex;
                cur_y = ey;
            }
            k = k + 1;
        }
        if (cur_x != sx || cur_y != sy) {
            out.push_back(sx);
            out.push_back(sy);
        }
        if (out.size() >= 6) contours->push_back(std::move(out));
        start = end + 1;
    }
}

TtfFont::Sdf TtfFont::rasterize(std::string_view utf8, float size_px,
                                float spread_px) const {
    Sdf sdf;
    if (size_px < 2.0f || utf8.empty()) return sdf;
    const float scale = size_px / static_cast<float>(units_per_em_);
    spread_px = std::max(spread_px, 2.0f);
    const float pad = std::ceil(spread_px) + 2.0f;

    // The pen advances in font units; contours land in px, y-up.
    std::vector<std::vector<float>> contours;
    float pen = 0.0f;
    uint32_t prev = 0;
    size_t cursor = 0;
    while (cursor < utf8.size()) {
        const uint32_t cp = utf8_decode(utf8.data(), utf8.size(), &cursor);
        const uint32_t gid = glyph_index(cp);
        if (prev) pen += kern_units(prev, gid);
        const float xf[6] = {1.0f, 0.0f, 0.0f, 1.0f, pen, 0.0f};
        append_outline(gid, xf, scale, &contours, 0);
        pen += advance_units(gid);
        prev = gid;
        if (pen * scale > static_cast<float>(kMaxBitmapW) - 2.0f * pad)
            break;   // overlong string truncates at the width cap
    }
    if (contours.empty()) return sdf;

    const float asc = ascent_units_ * scale;
    const float desc = descent_units_ * scale;   // negative
    const uint32_t w = std::min<uint32_t>(
        static_cast<uint32_t>(std::ceil(pen * scale + 2.0f * pad)),
        kMaxBitmapW);
    const uint32_t h = std::min<uint32_t>(
        static_cast<uint32_t>(std::ceil(asc - desc + 2.0f * pad)),
        kMaxBitmapH);
    if (w < 4 || h < 4) return sdf;
    const float baseline = pad + asc;

    // Flip y-up px to y-down bitmap.
    std::vector<Seg> segs;
    for (const std::vector<float>& c : contours) {
        const size_t n = c.size() / 2;
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            Seg s;
            s.x0 = c[i * 2] + pad;
            s.y0 = baseline - c[i * 2 + 1];
            s.x1 = c[j * 2] + pad;
            s.y1 = baseline - c[j * 2 + 1];
            if (s.y0 != s.y1 || s.x0 != s.x1) segs.push_back(s);
        }
    }

    // The fill rule is non-zero winding.
    std::vector<uint8_t> inside(static_cast<size_t>(w) * h, 0);
    std::vector<std::pair<float, int>> hits;
    for (uint32_t y = 0; y < h; ++y) {
        const float yc = static_cast<float>(y) + 0.5f;
        hits.clear();
        for (const Seg& s : segs) {
            const bool a = s.y0 <= yc, bb = s.y1 <= yc;
            if (a == bb) continue;
            const float t = (yc - s.y0) / (s.y1 - s.y0);
            hits.push_back({s.x0 + t * (s.x1 - s.x0), s.y1 > s.y0 ? 1 : -1});
        }
        std::sort(hits.begin(), hits.end());
        int wind = 0;
        float span_x = 0.0f;
        for (const auto& hit : hits) {
            if (wind != 0) {
                const int x0 = std::max(
                    0, static_cast<int>(std::ceil(span_x - 0.5f)));
                const int x1 = std::min(
                    static_cast<int>(w),
                    static_cast<int>(std::ceil(hit.first - 0.5f)));
                for (int x = x0; x < x1; ++x)
                    inside[static_cast<size_t>(y) * w +
                           static_cast<size_t>(x)] = 1;
            }
            if (wind == 0) span_x = hit.first;
            wind += hit.second;
            if (wind == 0) span_x = hit.first;
        }
    }

    // cell >= spread, so the 3x3 neighborhood holds every segment in range.
    const float cell = std::max(spread_px, 6.0f);
    const uint32_t gw = static_cast<uint32_t>(std::ceil(w / cell)) + 1;
    const uint32_t gh = static_cast<uint32_t>(std::ceil(h / cell)) + 1;
    std::vector<std::vector<uint32_t>> grid(static_cast<size_t>(gw) * gh);
    for (uint32_t si = 0; si < segs.size(); ++si) {
        const Seg& s = segs[si];
        const int cx0 = std::clamp(
            static_cast<int>(std::min(s.x0, s.x1) / cell), 0,
            static_cast<int>(gw) - 1);
        const int cx1 = std::clamp(
            static_cast<int>(std::max(s.x0, s.x1) / cell), 0,
            static_cast<int>(gw) - 1);
        const int cy0 = std::clamp(
            static_cast<int>(std::min(s.y0, s.y1) / cell), 0,
            static_cast<int>(gh) - 1);
        const int cy1 = std::clamp(
            static_cast<int>(std::max(s.y0, s.y1) / cell), 0,
            static_cast<int>(gh) - 1);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                grid[static_cast<size_t>(cy) * gw +
                     static_cast<size_t>(cx)].push_back(si);
    }
    auto seg_dist2 = [](const Seg& s, float x, float y) {
        const float vx = s.x1 - s.x0, vy = s.y1 - s.y0;
        const float wx = x - s.x0, wy = y - s.y0;
        const float len2 = vx * vx + vy * vy;
        const float t =
            len2 > 0.0f ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f)
                        : 0.0f;
        const float dx = wx - t * vx, dy = wy - t * vy;
        return dx * dx + dy * dy;
    };

    sdf.pixels.assign(static_cast<size_t>(w) * h, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const int cy = static_cast<int>(y / cell);
        for (uint32_t x = 0; x < w; ++x) {
            const int cx = static_cast<int>(x / cell);
            const float pxc = static_cast<float>(x) + 0.5f;
            const float pyc = static_cast<float>(y) + 0.5f;
            float d2 = spread_px * spread_px;
            for (int ny = cy - 1; ny <= cy + 1; ++ny) {
                if (ny < 0 || ny >= static_cast<int>(gh)) continue;
                for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                    if (nx < 0 || nx >= static_cast<int>(gw)) continue;
                    for (const uint32_t si :
                         grid[static_cast<size_t>(ny) * gw +
                              static_cast<size_t>(nx)])
                        d2 = std::min(d2, seg_dist2(segs[si], pxc, pyc));
                }
            }
            const float d = std::sqrt(d2);
            const float sd =
                inside[static_cast<size_t>(y) * w + x] ? d : -d;
            const float enc =
                std::clamp(0.5f + 0.5f * sd / spread_px, 0.0f, 1.0f);
            sdf.pixels[static_cast<size_t>(y) * w + x] =
                static_cast<uint8_t>(enc * 255.0f + 0.5f);
        }
    }
    sdf.width = w;
    sdf.height = h;
    sdf.spread_px = spread_px;
    return sdf;
}

}  // namespace looks::ui
