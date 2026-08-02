// Encoded space, and the hard saturate below is deliberate: the clipped
// halo rings are the point. Sharpen is the neutral version.

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
    float amount;
    float radius;   // px
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

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float2 r = radius / size;
    float3 blur = enc;
    blur += fx_srgb_oetf(saturate(input.SampleLevel(uv + float2(r.x, 0), 0).rgb));
    blur += fx_srgb_oetf(saturate(input.SampleLevel(uv - float2(r.x, 0), 0).rgb));
    blur += fx_srgb_oetf(saturate(input.SampleLevel(uv + float2(0, r.y), 0).rgb));
    blur += fx_srgb_oetf(saturate(input.SampleLevel(uv - float2(0, r.y), 0).rgb));
    blur += fx_srgb_oetf(saturate(input.SampleLevel(uv + r * 0.7071, 0).rgb));
    blur += fx_srgb_oetf(saturate(input.SampleLevel(uv - r * 0.7071, 0).rgb));
    blur += fx_srgb_oetf(saturate(
        input.SampleLevel(uv + float2(r.x, -r.y) * 0.7071, 0).rgb));
    blur += fx_srgb_oetf(saturate(
        input.SampleLevel(uv - float2(r.x, -r.y) * 0.7071, 0).rgb));
    blur /= 9.0;

    const float3 sharp = saturate(enc + (enc - blur) * amount);

    const float4 result = float4(fx_srgb_eotf(sharp), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
