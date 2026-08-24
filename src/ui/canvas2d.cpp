#include "ui/canvas2d.h"

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
    const Vec2 dir = (b - a).normalized_or_zero();
    if (dir == Vec2{}) return;
    const Vec2 n = dir.perp_ccw() * (thickness * 0.5f);
    const Vec2 pos[4] = {to_physical(a + n), to_physical(b + n),
                         to_physical(b - n), to_physical(a - n)};
    const Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const uint32_t c = color.to_rgba8();
    const uint32_t colors[4] = {c, c, c, c};
    const float shape[4] = {0, 0, 0, 0};
    emit_quad_physical(BatchKind::Solid, nullptr, pos, uv, colors, shape);
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
