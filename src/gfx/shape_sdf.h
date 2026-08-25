// CPU raster of a custom shape path into a small signed-distance field
// the generator kernel samples (R8: 0.5 = on the curve, distances in
// canvas-height units over +-kShapeSdfRange). Pure function of
// (path bytes, closed, aspect, w, h) - deterministic, no threading,
// no Vulkan, so the raster is unit-testable and export-stable.

#pragma once

#include <cstdint>
#include <vector>

#include "doc/document.h"

namespace looks::gfx {

// Half-range of the stored distances, canvas-height units. The kernel
// decodes with the same constant; feathers clamp to it.
inline constexpr float kShapeSdfRange = 0.25f;

// Fixed flatten subdivision per cubic span - the raster is a pure
// function of the path bytes, and the editor's segment hit tests map
// flattened samples back to (span, t) with the same constant.
inline constexpr int kShapeSubdiv = 16;

// Fixed-subdivision flatten of the cubic through the control points
// into a metric polyline (x scaled by aspect, y in canvas fractions),
// appended as x,y pairs. Closed paths wrap around. The monitor path
// editor and the raster share this so hit tests match pixels.
void shape_flatten(const std::vector<doc::PathPoint>& path, bool closed,
                   float aspect, std::vector<float>* out_xy);

// Rasterize into w*h bytes, row-major. Closed = signed fill under
// nonzero winding; open = unsigned distance to the stroke. An empty or
// degenerate path fills "far outside".
void shape_sdf_raster(const std::vector<doc::PathPoint>& path, bool closed,
                      float aspect, uint32_t w, uint32_t h,
                      std::vector<uint8_t>* out);

}  // namespace looks::gfx
