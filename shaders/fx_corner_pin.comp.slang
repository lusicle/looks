// Corner Pin: perspective quad warp. The frame is pinned by its four
// corners (each offset from its natural position in frame fractions) and
// output pixels inverse-map through the square-to-quad homography, so
// straight lines stay straight under the perspective. The uncovered
// region takes the edge policy; black gets the half-pixel analytic AA
// edge (same treatment as Transform).
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float wet;
    float opacity;
    uint seed;
    uint frame;
    float fps;
    float tl_x;        // corner offsets, frame fractions
    float tl_y;
    float tr_x;
    float tr_y;
    float bl_x;
    float bl_y;
    float br_x;
    float br_y;
    float edge_mode;   // 0 black, 1 clamp, 2 wrap, 3 mirror
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 dim = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / dim;
    const float4 base = input.SampleLevel(uv, 0);

    // Unit square -> pinned quad homography (projective square-to-quad
    // construction), then the adjugate inverts it without needing the
    // determinant. Every term is a handful of flops; nothing here is
    // worth a CPU precompute.
    const float2 p00 = float2(tl_x, tl_y);
    const float2 p10 = float2(1.0 + tr_x, tr_y);
    const float2 p01 = float2(bl_x, 1.0 + bl_y);
    const float2 p11 = float2(1.0 + br_x, 1.0 + br_y);

    const float2 e1 = p10 - p11;
    const float2 e2 = p01 - p11;
    const float2 sm = p00 - p10 + p11 - p01;
    const float det = e1.x * e2.y - e1.y * e2.x;
    float g = 0.0;
    float h = 0.0;
    if (abs(det) > 1.0e-9) {
        g = (sm.x * e2.y - sm.y * e2.x) / det;
        h = (e1.x * sm.y - e1.y * sm.x) / det;
    }
    const float a = p10.x - p00.x + g * p10.x;
    const float b = p01.x - p00.x + h * p01.x;
    const float c = p00.x;
    const float dq = p10.y - p00.y + g * p10.y;
    const float e = p01.y - p00.y + h * p01.y;
    const float f = p00.y;

    // adj(H) rows for H = [a b c; dq e f; g h 1].
    const float3 q = float3(uv, 1.0);
    const float3 r0 = float3(e - f * h, c * h - b, b * f - c * e);
    const float3 r1 = float3(f * g - dq, a - c * g, c * dq - a * f);
    const float3 r2 = float3(dq * h - e * g, b * g - a * h, a * e - b * dq);
    float sw = dot(r2, q);
    if (abs(sw) < 1.0e-12) sw = 1.0e-12;
    float2 suv = float2(dot(r0, q), dot(r1, q)) / sw;

    const int em = int(edge_mode + 0.5);
    float coverage = 1.0;
    if (em == 0) {
        // Black outside, with a half-pixel analytic AA edge.
        const float2 dpx = min(suv, 1.0 - suv) * dim;
        coverage = saturate(min(dpx.x, dpx.y) + 0.5);
        suv = clamp(suv, 0.0, 1.0);
    } else if (em == 2) {
        suv = frac(suv);
    } else if (em == 3) {
        // Triangle wave: 0..1..0 over a 2-frame period.
        suv = 1.0 - abs(frac(suv * 0.5) * 2.0 - 1.0);
    }
    // em == 1: the sampler clamps.

    float4 pinned = input.SampleLevel(suv, 0);
    pinned.rgb *= coverage;

    const float4 wetted = lerp(base, pinned, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
