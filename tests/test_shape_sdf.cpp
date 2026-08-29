#include "gfx/shape_sdf.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "test_framework.h"

using looks::doc::PathPoint;
using looks::gfx::shape_flatten;
using looks::gfx::shape_sdf_raster;

namespace {

std::vector<PathPoint> triangle() {
    // Zero tangents flatten to straight edges, so geometry is exact.
    std::vector<PathPoint> p(3);
    p[0].ax = 0.5f;
    p[0].ay = 0.2f;
    p[1].ax = 0.8f;
    p[1].ay = 0.8f;
    p[2].ax = 0.2f;
    p[2].ay = 0.8f;
    return p;
}

}  // namespace

TEST(shape_sdf_signs_and_determinism) {
    const std::vector<PathPoint> tri = triangle();
    std::vector<uint16_t> a, b;
    shape_sdf_raster(tri, /*closed=*/true, 16.0f / 9.0f, 240, 135, &a);
    shape_sdf_raster(tri, /*closed=*/true, 16.0f / 9.0f, 240, 135, &b);
    CHECK_EQ(a.size(), size_t{240 * 135});
    CHECK(a == b);

    auto at = [&](float u, float v) {
        const size_t x = static_cast<size_t>(u * 240.0f);
        const size_t y = static_cast<size_t>(v * 135.0f);
        return a[y * 240 + x];
    };
    // Encoded 0.5 is the boundary: inside is below, outside is above.
    CHECK(at(0.5f, 0.6f) < 30000);
    CHECK(at(0.02f, 0.02f) > 34000);
    CHECK(at(0.98f, 0.02f) > 34000);
    CHECK(at(0.5f, 0.76f) < at(0.5f, 0.84f));
}

TEST(shape_sdf_euclidean_far_field) {
    const std::vector<PathPoint> tri = triangle();
    std::vector<uint16_t> a;
    shape_sdf_raster(tri, /*closed=*/true, 1.0f, 256, 256, &a);
    auto dist_at = [&](float u, float v) {
        const size_t x = static_cast<size_t>(u * 256.0f);
        const size_t y = static_cast<size_t>(v * 256.0f);
        const float enc = a[y * 256 + x] / 65535.0f;
        return (enc - 0.5f) * 2.0f * looks::gfx::kShapeSdfRange;
    };
    for (int k = 1; k <= 4; ++k) {
        const float off = 0.03f * static_cast<float>(k);
        const float d = dist_at(0.5f, 0.8f + off);
        CHECK(std::fabs(d - off) < 0.006f);
    }
    const float d0 = dist_at(0.40f, 0.86f);
    const float d1 = dist_at(0.50f, 0.86f);
    const float d2 = dist_at(0.60f, 0.86f);
    CHECK(std::fabs(d0 - d1) < 0.004f);
    CHECK(std::fabs(d2 - d1) < 0.004f);
}

TEST(shape_sdf_open_stroke_and_empty) {
    std::vector<PathPoint> line(2);
    line[0].ax = 0.2f;
    line[0].ay = 0.5f;
    line[1].ax = 0.8f;
    line[1].ay = 0.5f;
    std::vector<uint16_t> s;
    shape_sdf_raster(line, /*closed=*/false, 1.0f, 128, 128, &s);
    auto at = [&](float u, float v) {
        return s[static_cast<size_t>(v * 128.0f) * 128 +
                 static_cast<size_t>(u * 128.0f)];
    };
    // An open path has no inside; on the stroke the value is near 0.5.
    CHECK(at(0.5f, 0.5f) < 36000);
    CHECK(at(0.5f, 0.05f) > 52000);
    CHECK(at(0.5f, 0.95f) > 52000);

    std::vector<uint16_t> e;
    shape_sdf_raster({}, true, 1.0f, 64, 64, &e);
    for (uint16_t v : e) CHECK_EQ(v, uint16_t{65535});
}

TEST(shape_flatten_subdivision_contract) {
    const std::vector<PathPoint> tri = triangle();
    std::vector<float> closed_poly, open_poly;
    shape_flatten(tri, true, 1.0f, &closed_poly);
    shape_flatten(tri, false, 1.0f, &open_poly);
    // Output points are xy-interleaved pairs.
    CHECK_EQ(closed_poly.size(),
             size_t{(1 + 3 * looks::gfx::kShapeSubdiv) * 2});
    CHECK_EQ(open_poly.size(),
             size_t{(1 + 2 * looks::gfx::kShapeSubdiv) * 2});
    CHECK(std::fabs(closed_poly[0] -
                    closed_poly[closed_poly.size() - 2]) < 1.0e-5f);
    CHECK(std::fabs(closed_poly[1] -
                    closed_poly[closed_poly.size() - 1]) < 1.0e-5f);
}
