#ifndef FX_OPTICS_SLANG
#define FX_OPTICS_SLANG
#include "fx_common.slang"

float4 fx_scatter(Sampler2D source, float2 uv, float2 radius)
{
    float4 sum = 0.0;
    float weight = 0.0;
    for (int i = 0; i < 16; ++i) {
        float t = (float(i) + 0.5) / 16.0;
        float a = float(i) * 2.3999632;
        float w = exp(-3.0 * t);
        float2 d = float2(cos(a), sin(a)) * sqrt(t) * radius;
        sum += source.SampleLevel(clamp(uv + d, 0.0, 1.0), 0) * w;
        weight += w;
    }
    return sum / weight;
}

float3 fx_film_halo(Sampler2D source, float2 uv, float2 radius, float hue, float sat)
{
    float wide = fx_luma(fx_scatter(source, uv, radius).rgb);
    float near = fx_luma(fx_scatter(source, uv, radius * 0.45).rgb);
    float a = hue * 6.2831853;
    float3 warm = float3(wide, near * (0.45 + 0.35 * cos(a)),
                        near * (0.18 + 0.3 * sin(a)));
    return lerp(float3(wide, wide, wide), max(warm, 0.0), saturate(sat));
}
#endif
