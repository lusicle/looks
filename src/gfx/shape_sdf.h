// R16 encoding: 0.5 = on the curve. Must stay a pure, deterministic function.

#pragma once

#include <cstdint>
#include <vector>

#include "doc/document.h"

namespace looks::gfx {

// Half-range in canvas-height units; the kernel decodes with this constant.
inline constexpr float kShapeSdfRange = 0.25f;

// Samples per cubic span; the editor's hit tests depend on the same value.
inline constexpr int kShapeSubdiv = 24;

// Output is x,y pairs: x scaled by aspect, y in canvas fractions.
// Closed paths wrap around.
void shape_flatten(const std::vector<doc::PathPoint>& path, bool closed,
                   float aspect, std::vector<float>* out_xy);

// Output is w*h R16 texels, row-major. Closed = signed fill, nonzero winding.
// Open = unsigned stroke distance. An empty path fills far outside.
void shape_sdf_raster(const std::vector<doc::PathPoint>& path, bool closed,
                      float aspect, uint32_t w, uint32_t h,
                      std::vector<uint16_t>* out);

}  // namespace looks::gfx
