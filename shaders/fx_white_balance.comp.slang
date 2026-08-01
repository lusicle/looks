// White Balance (primitive): temperature / tint as linear-light
// channel gains — the clean warm/cool control the roster only had as
// fixed Film Stock presets. Gains are normalized so mid-gray holds its
// luma (a temperature sweep re-tints, it does not re-expose).
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
    float temperature;   // -1 cool .. +1 warm
    float tint;          // -1 magenta .. +1 green
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    // Opposed r/b gains for temperature, g vs magenta for tint, then a
    // luma renormalize so the operation stays a chromatic shift.
    float3 gain = float3(1.0 + 0.5 * temperature,
                         1.0 + 0.35 * tint,
                         1.0 - 0.5 * temperature);
    const float luma_gain =
        max(fx_luma(gain), 1.0e-4);
    gain /= luma_gain;
    const float3 balanced = max(base.rgb * gain, 0.0);

    const float4 wetted =
        lerp(base, float4(balanced, base.a), saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
