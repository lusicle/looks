// Soft low-res upscale (spec §6.5): the look of footage decimated to a
// fraction of its resolution and stretched back up — bilinear mush between
// fat texels, with softness sweeping from blocky-nearest to fully smeared.
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
    float factor;     // decimation factor (source pixels per fat texel)
    float softness;   // 0 = blocky nearest, 1 = extra-soft bilinear
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float3 tap(float2 grid_uv)
{
    return input.SampleLevel(clamp(grid_uv, 0.0, 1.0), 0).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float f = max(factor, 2.0);
    const float2 low = size / f;                 // low-res grid dims
    const float2 g = uv * low - 0.5;
    const float2 cell = floor(g);
    const float2 frac_g = g - cell;

    // Four fat-texel centers, bilinear between them; softness widens the
    // interpolation window past the texel edge for the over-smoothed look.
    const float spread = 1.0 + softness * 1.5;
    const float3 c00 = tap((cell + 0.5) / low);
    const float3 c10 = tap((cell + float2(1.5, 0.5)) / low);
    const float3 c01 = tap((cell + float2(0.5, 1.5)) / low);
    const float3 c11 = tap((cell + 1.5) / low);
    float2 tw = saturate((frac_g - 0.5) / spread + 0.5);
    // Softness 0 sharpens toward nearest-neighbor steps.
    tw = lerp(step(0.5, frac_g), tw, saturate(softness * 2.0));
    const float3 up = lerp(lerp(c00, c10, tw.x), lerp(c01, c11, tw.x), tw.y);

    const float4 result = float4(up, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
