// Work in display-referred space, not in linear light.

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
    float palette;
    float pal_hue_a;
    float pal_hue_b;
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

    const float3 c = fx_srgb_oetf(saturate(base.rgb));
    const int pal = int(palette + 0.5);
    const float3 result =
        fx_palette_snap(pal, c, fx_luma(c), pal_hue_a, pal_hue_b);

    const float4 mapped = float4(fx_srgb_eotf(saturate(result)), base.a);
    const float4 wetted = lerp(base, mapped, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
