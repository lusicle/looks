// Palette Map (docs/flow_canvas.md v5.5 primitives): nearest-color snap
// to a fixed palette — the Quantizer's palette lock without the level
// snap or dither. Duotone ramps black -> hue A -> hue B -> white by
// luma. Display-referred core.
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
    float palette;    // 0 game boy, 1 cga, 2 nes, 3 teletext, 4 duotone
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
    float3 result;
    if (pal == 0)
    {
        // DMG greens, darkest to lightest.
        const float l = fx_luma(c);
        result = fx_nearest_of4(float3(l, l, l),
                                float3(0.059, 0.220, 0.059),
                                float3(0.188, 0.384, 0.188),
                                float3(0.545, 0.675, 0.059),
                                float3(0.608, 0.737, 0.059));
    }
    else if (pal == 1)
    {
        // CGA palette 1: black / cyan / magenta / white.
        result = fx_nearest_of4(c,
                                float3(0.0, 0.0, 0.0),
                                float3(0.333, 1.0, 1.0),
                                float3(1.0, 0.333, 1.0),
                                float3(1.0, 1.0, 1.0));
    }
    else if (pal == 2)
    {
        result = fx_nes_nearest(c);
    }
    else if (pal == 3)
    {
        // Teletext: each channel snaps to 0 or 1 (the 8 CEEFAX colors).
        result = round(c);
    }
    else
    {
        const float l = fx_luma(c);
        const float3 ha = fx_hue_to_rgb(pal_hue_a);
        const float3 hb = fx_hue_to_rgb(pal_hue_b);
        result = fx_nearest_of4(float3(l, l, l),
                                float3(0.02, 0.02, 0.02),
                                ha * 0.45,
                                hb * 0.85,
                                float3(0.98, 0.98, 0.98));
    }

    const float4 mapped = float4(fx_srgb_eotf(saturate(result)), base.a);
    const float4 wetted = lerp(base, mapped, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
