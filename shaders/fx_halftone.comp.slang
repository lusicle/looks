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
    const float footprint = canvas_w / float(width);
    const float filtered = saturate(v * g);
    if (c <= footprint * 1.5) return filtered;
    const float aa = footprint * 0.65 / c;
    const float resolved = smoothstep(1.5, 4.0, c / footprint);
    if (shp == 1u) {
        const float ca = cos(ang), sa = sin(ang);
        const float across = (-px.x * sa + px.y * ca + wob) / c;
        const float d = abs(frac(across) - 0.5);
        const float r = saturate(v) * 0.5 * g;
        const float e = max(s * 0.2, aa);
        return lerp(filtered, 1.0 - smoothstep(r - e, r + e, d), resolved);
    }
    if (shp == 2u) {
        const float2 dd = px - center;
        const float coord =
            length(dd) / c - atan2(dd.y, dd.x) * 0.15915494 + wob / c;
        const float d = abs(frac(coord) - 0.5);
        const float r = saturate(v) * 0.5 * g;
        const float e = max(s * 0.2, aa);
        return lerp(filtered, 1.0 - smoothstep(r - e, r + e, d), resolved);
    }
    const float ca = cos(ang), sa = sin(ang);
    const float2 rp = float2(px.x * ca - px.y * sa,
                             px.x * sa + px.y * ca + wob) / c;
    const float2 f = frac(rp) - 0.5;
    const float d = length(f);
    const bool inverse = filtered > 0.5;
    const float tone = inverse ? 1.0 - filtered : filtered;
    const float r = sqrt(tone * 0.318309886);
    const float distance = inverse ? length(abs(f) - 0.5) : d;
    const float e = max(s * 0.1, aa);
    const float coverage = 1.0 - smoothstep(r - e, r + e, distance);
    return lerp(filtered, inverse ? 1.0 - coverage : coverage, resolved);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float2 px = uv * float2(canvas_w, canvas_h);

    const float3 enc = fx_encoded(base);
    const float3 reflectance = saturate(fx_straight(base));
    const float base_ang = angle * 0.0174533;
    const uint m = uint(mode + 0.5);
    const uint shp = uint(shape + 0.5);
    const float2 center =
        float2(canvas_w, canvas_h) * 0.5;
    const float wob = fx_luma(enc) * wave * dot_px * 1.5;

    float3 outc;
    if (m < 2u) {
        const float black = fx_srgb_eotf(float3(0.02, 0.02, 0.02)).x;
        const float ink = screen_cov(px, base_ang, dot_px,
                                     (1.0 - fx_luma(reflectance)) / (1.0 - black), gain, soft, shp,
                                     center, wob);
        outc = lerp(float3(1.0, 1.0, 1.0), float3(black, black, black), ink);
        if (m == 1u) outc = 1.0 - outc;
    } else {
        // The screen angles are C 15, M 75, Y 0 and K 45 degrees.
        const float3 cmy = 1.0 - reflectance;
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
        outc *= lerp(float3(1.0, 1.0, 1.0), fx_srgb_eotf(float3(0.05, 0.90, 0.95)), cov_c);
        outc *= lerp(float3(1.0, 1.0, 1.0), fx_srgb_eotf(float3(0.90, 0.05, 0.90)), cov_m);
        outc *= lerp(float3(1.0, 1.0, 1.0), fx_srgb_eotf(float3(0.98, 0.95, 0.10)), cov_y);
        outc *= lerp(float3(1.0, 1.0, 1.0), fx_srgb_eotf(float3(0.03, 0.03, 0.03)), cov_k);
    }

    const float4 result = float4(outc * base.a, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
