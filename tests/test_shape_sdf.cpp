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
    // Corner points (zero tangents): flatten degenerates to straight
    // edges, so geometry is exact.
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
    std::vector<uint8_t> a, b;
    shape_sdf_raster(tri, /*closed=*/true, 16.0f / 9.0f, 240, 135, &a);
    shape_sdf_raster(tri, /*closed=*/true, 16.0f / 9.0f, 240, 135, &b);
    CHECK_EQ(a.size(), size_t{240 * 135});
    CHECK(a == b);   // pure function of the path bytes

    auto at = [&](float u, float v) {
        const size_t x = static_cast<size_t>(u * 240.0f);
        const size_t y = static_cast<size_t>(v * 135.0f);
        return a[y * 240 + x];
    };
    // Centroid is inside (< 0.5 encoded), frame corners far outside.
    CHECK(at(0.5f, 0.6f) < 120);
    CHECK(at(0.02f, 0.02f) > 135);
    CHECK(at(0.98f, 0.02f) > 135);
    // The boundary crosses 0.5 near an edge midpoint: probe just inside
    // vs just outside of the bottom edge (y = 0.8).
    CHECK(at(0.5f, 0.76f) < at(0.5f, 0.84f));
}

TEST(shape_sdf_open_stroke_and_empty) {
    std::vector<PathPoint> line(2);
    line[0].ax = 0.2f;
    line[0].ay = 0.5f;
    line[1].ax = 0.8f;
    line[1].ay = 0.5f;
    std::vector<uint8_t> s;
    shape_sdf_raster(line, /*closed=*/false, 1.0f, 128, 128, &s);
    auto at = [&](float u, float v) {
        return s[static_cast<size_t>(v * 128.0f) * 128 +
                 static_cast<size_t>(u * 128.0f)];
    };
    // On the stroke the distance is ~0 (encodes ~0.5); far away it
    // saturates high. Open paths carry no inside, so nothing < 0.5-ish.
    CHECK(at(0.5f, 0.5f) < 140);
    CHECK(at(0.5f, 0.05f) > 200);
    CHECK(at(0.5f, 0.95f) > 200);

    std::vector<uint8_t> e;
    shape_sdf_raster({}, true, 1.0f, 64, 64, &e);
    for (uint8_t v : e) CHECK_EQ(v, uint8_t{255});
}

TEST(shape_flatten_subdivision_contract) {
    const std::vector<PathPoint> tri = triangle();
    std::vector<float> closed_poly, open_poly;
    shape_flatten(tri, true, 1.0f, &closed_poly);
    shape_flatten(tri, false, 1.0f, &open_poly);
    // 1 + spans*kShapeSubdiv points, xy-interleaved.
    CHECK_EQ(closed_poly.size(),
             size_t{(1 + 3 * looks::gfx::kShapeSubdiv) * 2});
    CHECK_EQ(open_poly.size(),
             size_t{(1 + 2 * looks::gfx::kShapeSubdiv) * 2});
    // A closed flatten returns to its start.
    CHECK(std::fabs(closed_poly[0] -
                    closed_poly[closed_poly.size() - 2]) < 1.0e-5f);
    CHECK(std::fabs(closed_poly[1] -
                    closed_poly[closed_poly.size() - 1]) < 1.0e-5f);
}
