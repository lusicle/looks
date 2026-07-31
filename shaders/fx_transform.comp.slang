// Transform (docs/flow_canvas.md v5.5 primitives): mid-chain affine —
// scale / rotate / offset / flip with an edge policy for the uncovered
// region (black, clamp, wrap, mirror). Inverse-maps in pixel space so
// rotation is aspect-true; offsets are frame fractions.
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
    float scale;
    float angle;       // degrees
    float pos_x;       // frame fractions, -1..1
    float pos_y;
    float edge_mode;   // 0 black, 1 clamp, 2 wrap, 3 mirror
    float flip;        // 0 none, 1 h, 2 v, 3 both
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

    // Inverse map: undo offset, then rotation about frame center, then
    // scale, in pixel space.
    float2 p = float2(tid.xy) + 0.5 - float2(pos_x, pos_y) * dim;
    const float2 center = dim * 0.5;
    p -= center;
    const float a = -angle * 0.01745329252;
    const float ca = cos(a), sa = sin(a);
    p = float2(p.x * ca - p.y * sa, p.x * sa + p.y * ca);
    p /= max(scale, 0.05);
    p += center;

    float2 suv = p / dim;
    const int fl = int(flip + 0.5);
    if (fl == 1 || fl == 3) suv.x = 1.0 - suv.x;
    if (fl == 2 || fl == 3) suv.y = 1.0 - suv.y;

    const int em = int(edge_mode + 0.5);
    float coverage = 1.0;
    if (em == 0) {
        // Black outside, with a half-pixel analytic AA edge.
        const float2 d = min(suv, 1.0 - suv) * dim;
        coverage = saturate(min(d.x, d.y) + 0.5);
        suv = clamp(suv, 0.0, 1.0);
    } else if (em == 2) {
        suv = frac(suv);
    } else if (em == 3) {
        // Triangle wave: 0..1..0 over a 2-frame period.
        suv = 1.0 - abs(frac(suv * 0.5) * 2.0 - 1.0);
    }
    // em == 1: the sampler clamps.

    float4 moved = input.SampleLevel(suv, 0);
    moved.rgb *= coverage;

    const float4 wetted = lerp(base, moved, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
