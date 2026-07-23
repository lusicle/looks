// Canvas2D — immediate-mode CPU draw-list recorder (first-party
// re-implementation of the reference toolkit's canvas).
//
// Pure CPU: records vertices/indices/batches each frame; the Vulkan side
// (UiRenderer) turns batches into draw calls. Inputs are LOGICAL px; the
// per-frame scale is applied exactly once, at vertex write, so positions in
// the buffers are PHYSICAL px. Anti-aliasing is analytic — SDF params ride
// the per-vertex `shape` attribute and the fragment shader feathers edges
// with fwidth; no MSAA, no coverage geometry.

#pragma once

#include <cstdint>
#include <vector>

#include "ui/types.h"

namespace looks::ui {

struct UiTexture;  // opaque; defined and owned by UiRenderer

struct Vertex {
    Vec2 pos;         // physical px
    Vec2 uv;
    uint32_t color;   // packed RGBA8, sRGB-encoded RGB + linear A
    // {radius/type, stroke, halfW, halfH} in physical px. All zero on
    // pass-through primitives (solid rect / triangle / line / glyph).
    float shape[4];
};
static_assert(sizeof(Vertex) == 36, "vertex layout is shared by all UI pipelines");

enum class BatchKind : uint8_t { Solid, Text, Image };

struct Batch {
    Rect scissor_physical;   // empty() => scissor disabled (full framebuffer)
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    BatchKind kind = BatchKind::Solid;
    const UiTexture* texture = nullptr;   // Text/Image batches only
};

class Canvas2D {
public:
    Canvas2D();

    // Clears the recorded lists (buffers are reused, not freed) and sets the
    // frame's logical->physical scale.
    void begin_frame(float logical_to_physical, Vec2 viewport_logical);

    float scale() const { return scale_; }
    Vec2 viewport() const { return viewport_logical_; }
    bool pixel_snap() const { return pixel_snap_; }
    void set_pixel_snap(bool snap) { pixel_snap_ = snap; }

    // ---- primitives (logical px)
    void draw_rect(const Rect& r, Color color);
    void draw_rect_outline(const Rect& r, float stroke, Color color);
    void draw_gradient_quad(const Rect& r, Color tl, Color tr, Color br, Color bl);
    void draw_triangle(Vec2 a, Vec2 b, Vec2 c, Color color);
    void draw_line(Vec2 a, Vec2 b, float thickness, Color color);
    void draw_sdf_rect(const Rect& r, float radius, Color color);
    void draw_sdf_rect_outline(const Rect& r, float radius, float stroke, Color color);

    // Glyph quad in logical px with normalized atlas UVs (Text batch).
    void draw_glyph_quad(const Rect& r, float u0, float v0, float u1, float v1,
                         Color color, const UiTexture* atlas);

    // Textured quad; radius > 0 applies the SDF rounded-corner mask.
    void draw_image_quad(const Rect& r, const UiTexture* texture,
                         float u0, float v0, float u1, float v1,
                         Color tint, float corner_radius = 0.0f);

    // ---- clipping (scissor stack; rects intersect with the parent top)
    void push_clip(const Rect& logical);
    void pop_clip();
    Rect current_clip_physical() const;   // empty() => unclipped

    // ---- renderer access
    const std::vector<Vertex>& vertices() const { return vertices_; }
    const std::vector<uint16_t>& indices() const { return indices_; }
    const std::vector<Batch>& batches() const { return batches_; }
    uint32_t total_index_count() const { return static_cast<uint32_t>(indices_.size()); }

private:
    bool clip_collapsed() const;
    Batch& current_batch(BatchKind kind, const UiTexture* texture);
    // Emits TL,TR,BR,BL + two triangles. Positions already physical.
    void emit_quad_physical(BatchKind kind, const UiTexture* texture,
                            const Vec2 (&pos)[4], const Vec2 (&uv)[4],
                            const uint32_t (&color)[4], const float (&shape)[4]);
    Vec2 to_physical(Vec2 logical) const {
        return {logical.x * scale_, logical.y * scale_};
    }

    std::vector<Vertex> vertices_;
    std::vector<uint16_t> indices_;
    std::vector<Batch> batches_;
    std::vector<Rect> clip_stack_;   // physical px
    float scale_ = 1.0f;
    Vec2 viewport_logical_{};
    bool pixel_snap_ = true;
    bool warned_overflow_ = false;
};

}  // namespace looks::ui
