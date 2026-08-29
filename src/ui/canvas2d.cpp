#include "ui/canvas2d.h"

#include <algorithm>
#include <cmath>

#include "util/log.h"

namespace looks::ui {

namespace {
constexpr uint32_t kMaxVertices = 0xFFFF;   // u16 index ceiling
}

Canvas2D::Canvas2D() {
    vertices_.reserve(1024);
    indices_.reserve(1536);
    batches_.reserve(32);
}

void Canvas2D::begin_frame(float logical_to_physical, Vec2 viewport_logical) {
    vertices_.clear();
    indices_.clear();
    batches_.clear();
    clip_stack_.clear();
    scale_ = logical_to_physical > 1e-3f ? logical_to_physical : 1e-3f;
    viewport_logical_ = viewport_logical;
}

bool Canvas2D::clip_collapsed() const {
    return !clip_stack_.empty() && clip_stack_.back().empty();
}

Rect Canvas2D::current_clip_physical() const {
    return clip_stack_.empty() ? Rect{} : clip_stack_.back();
}

void Canvas2D::push_clip(const Rect& logical) {
    Rect physical{logical.x * scale_, logical.y * scale_,
                  logical.w * scale_, logical.h * scale_};
    if (!clip_stack_.empty() && !clip_stack_.back().empty())
        physical = physical.intersect(clip_stack_.back());
    clip_stack_.push_back(physical);
}

void Canvas2D::pop_clip() {
    if (!clip_stack_.empty()) clip_stack_.pop_back();
}

Batch& Canvas2D::current_batch(BatchKind kind, const UiTexture* texture) {
    const Rect scissor = current_clip_physical();
    if (!batches_.empty()) {
        Batch& last = batches_.back();
        if (last.kind == kind && last.texture == texture &&
            last.scissor_physical == scissor)
            return last;
    }
    Batch batch;
    batch.scissor_physical = scissor;
    batch.first_index = static_cast<uint32_t>(indices_.size());
    batch.index_count = 0;
    batch.kind = kind;
    batch.texture = texture;
    batches_.push_back(batch);
    return batches_.back();
}

void Canvas2D::emit_quad_physical(BatchKind kind, const UiTexture* texture,
                                  const Vec2 (&pos)[4], const Vec2 (&uv)[4],
                                  const uint32_t (&color)[4],
                                  const float (&shape)[4]) {
    if (clip_collapsed()) return;
    if (vertices_.size() + 4 > kMaxVertices) {
        if (!warned_overflow_) {
            log_warn("ui: 16-bit vertex budget exceeded — dropping quads");
            warned_overflow_ = true;
        }
        return;
    }
    Batch& batch = current_batch(kind, texture);
    const uint16_t base = static_cast<uint16_t>(vertices_.size());
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.pos = pos[i];
        v.uv = uv[i];
        v.color = color[i];
        v.shape[0] = shape[0];
        v.shape[1] = shape[1];
        v.shape[2] = shape[2];
        v.shape[3] = shape[3];
        vertices_.push_back(v);
    }
    const uint16_t quad_indices[6] = {
        base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
        base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)};
    indices_.insert(indices_.end(), quad_indices, quad_indices + 6);
    batch.index_count += 6;
}

void Canvas2D::draw_rect(const Rect& r, Color color) {
    if (r.empty()) return;
    const Vec2 tl = to_physical({r.x, r.y});
    const Vec2 br = to_physical({r.right(), r.bottom()});
    const Vec2 pos[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    const Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c = color.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {0, 0, 0, 0};
    emit_quad_physical(BatchKind::Solid, nullptr, pos, uv, colors, shape);
}

void Canvas2D::draw_rect_corners(const Rect& r, Color c00, Color c10,
                                 Color c11, Color c01) {
    if (r.empty()) return;
    const Vec2 tl = to_physical({r.x, r.y});
    const Vec2 br = to_physical({r.right(), r.bottom()});
    const Vec2 pos[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    const Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t colors[4] = {c00.to_rgba8(), c10.to_rgba8(),
                                c11.to_rgba8(), c01.to_rgba8()};
    const float shape[4] = {0, 0, 0, 0};
    emit_quad_physical(BatchKind::Solid, nullptr, pos, uv, colors, shape);
}

void Canvas2D::draw_rect_outline(const Rect& r, float stroke, Color color) {
    if (r.empty() || stroke <= 0.0f) return;
    // Collapses to a fill when the stroke swallows the interior.
    if (stroke * 2.0f >= r.w || stroke * 2.0f >= r.h) {
        draw_rect(r, color);
        return;
    }
    draw_rect({r.x, r.y, r.w, stroke}, color);                                  // top
    draw_rect({r.x, r.bottom() - stroke, r.w, stroke}, color);                  // bottom
    draw_rect({r.x, r.y + stroke, stroke, r.h - 2 * stroke}, color);            // left
    draw_rect({r.right() - stroke, r.y + stroke, stroke, r.h - 2 * stroke},     // right
              color);
}

void Canvas2D::draw_triangle(Vec2 a, Vec2 b, Vec2 c, Color color) {
    if (clip_collapsed()) return;
    if (vertices_.size() + 3 > kMaxVertices) return;
    Batch& batch = current_batch(BatchKind::Solid, nullptr);
    const uint16_t base = static_cast<uint16_t>(vertices_.size());
    const uint32_t packed = color.to_rgba8();
    for (Vec2 p : {a, b, c}) {
        Vertex v;
        v.pos = to_physical(p);
        v.uv = {0, 0};
        v.color = packed;
        v.shape[0] = v.shape[1] = v.shape[2] = v.shape[3] = 0;
        vertices_.push_back(v);
    }
    indices_.push_back(base);
    indices_.push_back(static_cast<uint16_t>(base + 1));
    indices_.push_back(static_cast<uint16_t>(base + 2));
    batch.index_count += 3;
}

void Canvas2D::draw_line(Vec2 a, Vec2 b, float thickness, Color color) {
    if (thickness <= 0.0f) return;
    // An SDF capsule: a rotated quad whose shape attributes carry the
    // segment's true half extents while the quad itself is padded one
    // physical pixel for the feather ramp. The rounded-box SDF with
    // radius = half thickness gives analytic anti-aliasing and round
    // caps, so chained polyline segments join without notches and
    // sub-pixel thicknesses fade instead of flickering. The fragment
    // shader reconstructs local position from UV * half_size, so the
    // padded corners must map UV past [0,1] by pad/extent.
    const Vec2 pa = to_physical(a);
    const Vec2 pb = to_physical(b);
    const Vec2 d{pb.x - pa.x, pb.y - pa.y};
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len <= 0.0f) return;
    const Vec2 dir{d.x / len, d.y / len};
    const Vec2 nrm{-dir.y, dir.x};
    const float hl = len * 0.5f;
    const float ht = thickness * 0.5f * scale_;
    const float pad = 1.0f;
    const Vec2 center{(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f};
    const Vec2 ex{dir.x * (hl + pad), dir.y * (hl + pad)};
    const Vec2 ey{nrm.x * (ht + pad), nrm.y * (ht + pad)};
    const Vec2 pos[4] = {{center.x - ex.x - ey.x, center.y - ex.y - ey.y},
                         {center.x + ex.x - ey.x, center.y + ex.y - ey.y},
                         {center.x + ex.x + ey.x, center.y + ex.y + ey.y},
                         {center.x - ex.x + ey.x, center.y - ex.y + ey.y}};
    const float ux = (hl + pad) / (2.0f * hl);
    const float uy = (ht + pad) / (2.0f * ht);
    const Vec2 uv[4] = {{0.5f - ux, 0.5f - uy},
                        {0.5f + ux, 0.5f - uy},
                        {0.5f + ux, 0.5f + uy},
                        {0.5f - ux, 0.5f + uy}};
    const uint32_t c = color.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {std::min(ht, hl), 0.0f, hl, ht};
    emit_quad_physical(BatchKind::Solid, nullptr, pos, uv, colors, shape);
}

void Canvas2D::draw_polyline(const Vec2* pts, int count, float thickness,
                             Color color, bool closed) {
    if (!pts || count < 2 || thickness <= 0.0f) return;
    if (clip_collapsed()) return;
    // Physical points, consecutive duplicates dropped (a zero-length
    // span has no normal).
    std::vector<Vec2> p;
    p.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const Vec2 q = to_physical(pts[i]);
        if (!p.empty()) {
            const float dx = q.x - p.back().x;
            const float dy = q.y - p.back().y;
            if (dx * dx + dy * dy < 1e-6f) continue;
        }
        p.push_back(q);
    }
    const size_t n = p.size();
    if (n < 2) return;
    if (closed && n < 3) closed = false;
    if (vertices_.size() + n * 4 > kMaxVertices) {
        if (!warned_overflow_) {
            log_warn("ui: 16-bit vertex budget exceeded — dropping quads");
            warned_overflow_ = true;
        }
        return;
    }
    // A sub-pixel stroke keeps a half-pixel core and fades by width
    // instead of flickering.
    float alpha = color.a;
    float hw = thickness * 0.5f * scale_;
    if (hw < 0.5f) {
        alpha *= hw * 2.0f;
        hw = 0.5f;
    }
    const float fringe = 1.0f;
    const uint32_t core = color.with_alpha(alpha).to_rgba8();
    const uint32_t edge = color.with_alpha(0.0f).to_rgba8();

    auto seg_normal = [&](size_t i) -> Vec2 {
        const Vec2& a = p[i];
        const Vec2& b = p[(i + 1) % n];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        return {-dy / len, dx / len};
    };

    Batch& batch = current_batch(BatchKind::Solid, nullptr);
    const uint16_t base = static_cast<uint16_t>(vertices_.size());
    const float shape[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        // The joint normal averages its two segments' normals; the
        // miter scale keeps the stroke width constant through the bend,
        // clamped so a hairpin cannot spike.
        Vec2 m;
        if (!closed && i == 0) {
            m = seg_normal(0);
        } else if (!closed && i == n - 1) {
            m = seg_normal(n - 2);
        } else {
            const Vec2 n0 = seg_normal((i + n - 1) % n);
            const Vec2 n1 = seg_normal(i);
            m = {n0.x + n1.x, n0.y + n1.y};
            const float len = std::sqrt(m.x * m.x + m.y * m.y);
            if (len < 1e-4f) {
                m = n1;
            } else {
                m = {m.x / len, m.y / len};
                const float d = m.x * n1.x + m.y * n1.y;
                const float s = 1.0f / std::max(d, 0.5f);
                m = {m.x * s, m.y * s};
            }
        }
        const Vec2 in{m.x * hw, m.y * hw};
        const Vec2 out{m.x * (hw + fringe), m.y * (hw + fringe)};
        const Vec2 pos[4] = {{p[i].x + out.x, p[i].y + out.y},
                             {p[i].x + in.x, p[i].y + in.y},
                             {p[i].x - in.x, p[i].y - in.y},
                             {p[i].x - out.x, p[i].y - out.y}};
        const uint32_t cols[4] = {edge, core, core, edge};
        for (int v = 0; v < 4; ++v) {
            Vertex vt;
            vt.pos = pos[v];
            vt.uv = {0, 0};
            vt.color = cols[v];
            vt.shape[0] = shape[0];
            vt.shape[1] = shape[1];
            vt.shape[2] = shape[2];
            vt.shape[3] = shape[3];
            vertices_.push_back(vt);
        }
    }
    const size_t spans = closed ? n : n - 1;
    for (size_t i = 0; i < spans; ++i) {
        const uint16_t a = static_cast<uint16_t>(base + i * 4);
        const uint16_t b =
            static_cast<uint16_t>(base + ((i + 1) % n) * 4);
        for (int row = 0; row < 3; ++row) {
            const uint16_t a0 = static_cast<uint16_t>(a + row);
            const uint16_t a1 = static_cast<uint16_t>(a + row + 1);
            const uint16_t b0 = static_cast<uint16_t>(b + row);
            const uint16_t b1 = static_cast<uint16_t>(b + row + 1);
            const uint16_t quad[6] = {a0, b0, b1, a0, b1, a1};
            indices_.insert(indices_.end(), quad, quad + 6);
            batch.index_count += 6;
        }
    }
}

void Canvas2D::draw_sdf_rect(const Rect& r, float radius, Color color) {
    if (r.empty()) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    if (radius <= 0.0f) {
        draw_rect(r, color);
        return;
    }
    const Vec2 tl = to_physical({r.x, r.y});
    const Vec2 br = to_physical({r.right(), r.bottom()});
    const float half_w = (br.x - tl.x) * 0.5f;
    const float half_h = (br.y - tl.y) * 0.5f;
    const Vec2 pos[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    const Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c = color.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {radius * scale_, 0.0f, half_w, half_h};
    emit_quad_physical(BatchKind::Solid, nullptr, pos, uv, colors, shape);
}

void Canvas2D::draw_sdf_rect_outline(const Rect& r, float radius, float stroke,
                                     Color color) {
    if (r.empty() || stroke <= 0.0f) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    const Vec2 tl = to_physical({r.x, r.y});
    const Vec2 br = to_physical({r.right(), r.bottom()});
    const float half_w = (br.x - tl.x) * 0.5f;
    const float half_h = (br.y - tl.y) * 0.5f;
    const Vec2 pos[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    const Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c = color.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {radius * scale_, stroke * scale_, half_w, half_h};
    emit_quad_physical(BatchKind::Solid, nullptr, pos, uv, colors, shape);
}

void Canvas2D::draw_glyph_quad(const Rect& r, float u0, float v0, float u1,
                               float v1, Color color, const UiTexture* atlas) {
    if (r.empty() || !atlas) return;
    const Vec2 tl = to_physical({r.x, r.y});
    const Vec2 br = to_physical({r.right(), r.bottom()});
    const Vec2 pos[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    const Vec2 uv[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    const uint32_t c = color.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {0, 0, 0, 0};
    emit_quad_physical(BatchKind::Text, atlas, pos, uv, colors, shape);
}

void Canvas2D::draw_image_quad(const Rect& r, const UiTexture* texture,
                               float u0, float v0, float u1, float v1,
                               Color tint, float corner_radius) {
    if (r.empty() || !texture) return;
    corner_radius = std::min(corner_radius, std::min(r.w, r.h) * 0.5f);
    const Vec2 tl = to_physical({r.x, r.y});
    const Vec2 br = to_physical({r.right(), r.bottom()});
    const float half_w = (br.x - tl.x) * 0.5f;
    const float half_h = (br.y - tl.y) * 0.5f;
    const Vec2 pos[4] = {tl, {br.x, tl.y}, br, {tl.x, br.y}};
    const Vec2 uv[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    const uint32_t c = tint.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {corner_radius > 0.0f ? corner_radius * scale_ : 0.0f,
                            0.0f, half_w, half_h};
    emit_quad_physical(BatchKind::Image, texture, pos, uv, colors, shape);
}

}  // namespace looks::ui
