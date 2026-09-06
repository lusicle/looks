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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float strength;
    float angle;
    float mode;
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

    const float2 dx = float2(1.0 / canvas_w, 0.0);
    const float2 dy = float2(0.0, 1.0 / canvas_h);
    const float gx = fx_luma(fx_straight(input.SampleLevel(clamp(uv + dx, 0.0, 1.0), 0))) -
                     fx_luma(fx_straight(input.SampleLevel(clamp(uv - dx, 0.0, 1.0), 0)));
    const float gy = fx_luma(fx_straight(input.SampleLevel(clamp(uv + dy, 0.0, 1.0), 0))) -
                     fx_luma(fx_straight(input.SampleLevel(clamp(uv - dy, 0.0, 1.0), 0)));
    const float3 normal = normalize(float3(-gx * strength * 4.0, -gy * strength * 4.0, 1.0));
    const float3 light = normalize(float3(cos(angle), sin(angle), 1.0));
    const float relief = max(dot(normal, light), 0.0) - 0.70710678;

    float3 outc;
    if (mode >= 0.5)
        outc = max(fx_straight(base) + relief, 0.0);
    else
        outc = max(fx_srgb_eotf(float3(0.5, 0.5, 0.5)) + relief, 0.0);

    const float4 result = float4(outc * base.a, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
