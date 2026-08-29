// The screen runs in the encoded domain, not in linear light.

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
    float dot_px;
    float angle;    // degrees
    float mode;
    float gain;
    float soft;
    float shape;
    float wave;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float screen_cov(float2 px, float ang, float cell, float v, float g,
                 float s, uint shp, float2 center, float wob)
{
    const float c = max(cell, 1.0);
    if (shp == 1u) {
        const float ca = cos(ang), sa = sin(ang);
        const float across = (-px.x * sa + px.y * ca + wob) / c;
        const float d = abs(frac(across) - 0.5);
        const float r = saturate(v) * 0.5 * g;
        const float e = s * 0.2 + 0.02;
        return 1.0 - smoothstep(r - e, r + e, d);
    }
    if (shp == 2u) {
        const float2 dd = px - center;
        const float coord =
            length(dd) / c - atan2(dd.y, dd.x) * 0.15915494 + wob / c;
        const float d = abs(frac(coord) - 0.5);
        const float r = saturate(v) * 0.5 * g;
        const float e = s * 0.2 + 0.02;
        return 1.0 - smoothstep(r - e, r + e, d);
    }
    const float ca = cos(ang), sa = sin(ang);
    const float2 rp = float2(px.x * ca - px.y * sa,
                             px.x * sa + px.y * ca + wob) / c;
    const float2 f = frac(rp) - 0.5;
    const float d = length(f);
    // Radius 0.72 lets full tone reach the cell corner at 0.707.
    const float r = sqrt(saturate(v)) * 0.72 * g;
    const float e = s * 0.35 + 0.02;
    return 1.0 - smoothstep(r - e, r + e, d);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float2 px = float2(tid.xy);

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float base_ang = angle * 0.0174533;
    const uint m = uint(mode + 0.5);
    const uint shp = uint(shape + 0.5);
    const float2 center =
        float2(float(width) * 0.5, float(height) * 0.5);
    const float wob = fx_luma(enc) * wave * dot_px * 1.5;

    float3 outc;
    if (m < 2u) {
        const float ink = screen_cov(px, base_ang, dot_px,
                                     1.0 - fx_luma(enc), gain, soft, shp,
                                     center, wob);
        outc = lerp(float3(1.0, 1.0, 1.0), float3(0.02, 0.02, 0.02), ink);
        if (m == 1u) outc = 1.0 - outc;
    } else {
        // The screen angles are C 15, M 75, Y 0 and K 45 degrees.
        const float3 cmy = 1.0 - enc;
        const float k = min(min(cmy.x, cmy.y), cmy.z);
        const float denom = max(1.0 - k, 1.0e-3);
        const float3 sep = saturate((cmy - k) / denom);
        const float cov_c = screen_cov(px, base_ang + 0.2618, dot_px, sep.x,
                                       gain, soft, shp, center, wob);
        const float cov_m = screen_cov(px, base_ang + 1.3090, dot_px, sep.y,
                                       gain, soft, shp, center, wob);
        const float cov_y = screen_cov(px, base_ang, dot_px, sep.z, gain,
                                       soft, shp, center, wob);
        const float cov_k = screen_cov(px, base_ang + 0.7854, dot_px, k,
                                       gain, soft, shp, center, wob);
        outc = float3(1.0, 1.0, 1.0);
        outc *= 1.0 - cov_c * float3(0.95, 0.10, 0.05);
        outc *= 1.0 - cov_m * float3(0.10, 0.95, 0.10);
        outc *= 1.0 - cov_y * float3(0.02, 0.05, 0.90);
        outc *= 1.0 - cov_k * float3(0.97, 0.97, 0.97);
    }

    const float4 result = float4(fx_srgb_eotf(saturate(outc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
