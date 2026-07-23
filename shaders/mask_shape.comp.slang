// Shape mask (spec §8). Two modes:
//  - procedural ellipse<->rectangle morph with analytic feather;
//  - bezier path (path_count >= 3): a closed Catmull-Rom through the
//    user's points, flattened engine-side into an Nx1 RG32F polyline —
//    point-in-polygon fill with signed-distance feather.
// Coordinates normalized to the frame.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float center_x;
    float center_y;
    float radius_x;
    float radius_y;
    float roundness;   // 1 = ellipse, 0 = rectangle
    float feather;     // normalized distance units
    uint invert;
    uint path_count;   // flattened polyline points; 0 = procedural mode
};

[[vk::binding(0, 0)]] Sampler2D path;   // Nx1 RG32F flattened polyline
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float2 path_at(uint i)
{
    const float2 uv = (float2(float(i) + 0.5, 0.5)) /
                      float2(float(path_count), 1.0);
    return path.SampleLevel(uv, 0).rg;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float soft = max(feather, 1.0e-4);

    float m;
    if (path_count >= 3) {
        // Crossing-number fill + min distance to the polyline for feather.
        bool inside = false;
        float min_d = 1.0e9;
        float2 a = path_at(path_count - 1);
        for (uint i = 0; i < path_count; ++i) {
            const float2 b = path_at(i);
            if ((a.y > uv.y) != (b.y > uv.y)) {
                const float x =
                    a.x + (uv.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (x > uv.x) inside = !inside;
            }
            const float2 ab = b - a;
            const float2 ap = uv - a;
            const float t =
                clamp(dot(ap, ab) / max(dot(ab, ab), 1.0e-12), 0.0, 1.0);
            min_d = min(min_d, length(ap - ab * t));
            a = b;
        }
        const float sd = inside ? -min_d : min_d;
        m = 1.0 - smoothstep(-soft, soft, sd);
    } else {
        const float2 d = uv - float2(center_x, center_y);
        const float2 r = max(float2(radius_x, radius_y), 1.0e-4);
        const float2 q = abs(d) / r;
        const float ellipse_d = length(q);
        const float rect_d = max(q.x, q.y);
        const float dist = lerp(rect_d, ellipse_d, saturate(roundness));
        m = 1.0 - smoothstep(1.0 - soft, 1.0 + soft, dist);
    }
    if (invert != 0)
        m = 1.0 - m;
    output[tid.xy] = float4(m, m, m, 1.0);
}
