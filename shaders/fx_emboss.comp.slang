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
    float strength;
    float angle;
    float mode;     // 0 gray relief, 1 colored relief
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

    const float2 dir = float2(cos(angle), sin(angle)) / size;
    const float3 ahead = input.SampleLevel(clamp(uv + dir, 0.0, 1.0), 0).rgb;
    const float3 behind = input.SampleLevel(clamp(uv - dir, 0.0, 1.0), 0).rgb;
    const float relief =
        (fx_luma(behind) - fx_luma(ahead)) * strength * 2.0;

    float3 outc;
    if (mode >= 0.5)
        outc = max(base.rgb + relief, 0.0);
    else
        outc = fx_srgb_eotf(saturate(float3(0.5, 0.5, 0.5) + relief));

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
