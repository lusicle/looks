// Inverse-map in pixel space, not uv, so rotation stays aspect-true.

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
    float angle;
    float pos_x;
    float pos_y;
    float edge_mode;
    float flip;
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
        const float2 d = min(suv, 1.0 - suv) * dim;
        coverage = saturate(min(d.x, d.y) + 0.5);
        suv = clamp(suv, 0.0, 1.0);
    } else if (em == 2) {
        suv = frac(suv);
    } else if (em == 3) {
        suv = 1.0 - abs(frac(suv * 0.5) * 2.0 - 1.0);
    }
    // em == 1 needs no case. The sampler already clamps.

    float4 moved = input.SampleLevel(suv, 0) * coverage;

    const float4 wetted = lerp(base, moved, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
