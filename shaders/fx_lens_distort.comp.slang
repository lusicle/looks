// Lens distortion (spec §6.3): Brown-Conrady radial model on the sampling
// position — positive k1 barrels (edges bow out), negative pincushions.
// Aspect-corrected so distortion stays circular on screen.
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
    float k1;      // r^2 coefficient
    float k2;      // r^4 coefficient
    float zoom;    // compensating scale
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float aspect = size.x / size.y;
    float2 d = (uv - 0.5) * float2(aspect, 1.0) * 2.0;
    const float r2 = dot(d, d);
    const float f = 1.0 + k1 * r2 + k2 * r2 * r2;
    d *= f / max(zoom, 1.0e-3);

    const float2 suv =
        clamp(d * 0.5 / float2(aspect, 1.0) + 0.5, 0.0, 1.0);
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
