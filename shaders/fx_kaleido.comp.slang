// Kaleidoscope / mirror symmetry (spec §6.2): fold the angle around N
// wedges about a movable center; every wedge samples the first, mirrored
// at the seam so edges meet cleanly.
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
    float segments;    // wedge count, >= 2
    float angle;       // rotation of the fold
    float center_x;    // normalized center
    float center_y;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float kPi = 3.14159265358979;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 center = float2(center_x, center_y) * size;
    const float2 d = float2(tid.xy) + 0.5 - center;
    const float r = length(d);

    const float seg = 2.0 * kPi / clamp(round(segments), 2.0, 32.0);
    float a = atan2(d.y, d.x) + angle;
    a = a - seg * floor(a / seg);          // wrap into one wedge
    if (a > seg * 0.5) a = seg - a;        // mirror at the wedge seam
    a -= angle;

    const float2 sample_px = center + r * float2(cos(a), sin(a));
    const float2 suv = clamp(sample_px / size, 0.0, 1.0);
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
