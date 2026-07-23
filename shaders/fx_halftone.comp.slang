// Halftone (print sim): angled dot screens with dot gain. Beyond the glyph
// engine's grid dots (spec §6.6) — the screens rotate, and CMYK mode runs
// four screens at classic press angles so their interference produces the
// authentic rosette. Display-referred: dots live on the print.
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
    float dot_px;   // screen cell size
    float angle;    // screen angle, degrees (offsets every screen)
    float mode;     // 0 ink-on-paper, 1 paper-on-ink, 2 cmyk
    float gain;     // dot gain (radius multiplier)
    float soft;     // dot edge softness
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Coverage of a rotated dot screen at pixel `px` for tone value `v`
// (0 = no ink, 1 = solid).
float screen_cov(float2 px, float ang, float cell, float v, float g,
                 float s)
{
    const float ca = cos(ang), sa = sin(ang);
    const float2 rp = float2(px.x * ca - px.y * sa,
                             px.x * sa + px.y * ca) / max(cell, 1.0);
    const float2 f = frac(rp) - 0.5;
    const float d = length(f);
    // Solid tone must reach the cell corner (~0.707).
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

    float3 outc;
    if (m < 2u) {
        // Mono: darkness drives ink on white paper (mode 1 inverts).
        const float ink =
            screen_cov(px, base_ang, dot_px, 1.0 - fx_luma(enc), gain, soft);
        outc = lerp(float3(1.0, 1.0, 1.0), float3(0.02, 0.02, 0.02), ink);
        if (m == 1u) outc = 1.0 - outc;
    } else {
        // CMYK: classic press angles C15 M75 Y0 K45 (relative to `angle`),
        // GCR black, multiplicative ink laydown on white.
        const float3 cmy = 1.0 - enc;
        const float k = min(min(cmy.x, cmy.y), cmy.z);
        const float denom = max(1.0 - k, 1.0e-3);
        const float3 sep = saturate((cmy - k) / denom);
        const float cov_c = screen_cov(px, base_ang + 0.2618, dot_px, sep.x,
                                       gain, soft);
        const float cov_m = screen_cov(px, base_ang + 1.3090, dot_px, sep.y,
                                       gain, soft);
        const float cov_y = screen_cov(px, base_ang, dot_px, sep.z, gain,
                                       soft);
        const float cov_k = screen_cov(px, base_ang + 0.7854, dot_px, k,
                                       gain, soft);
        outc = float3(1.0, 1.0, 1.0);
        outc *= 1.0 - cov_c * float3(0.95, 0.10, 0.05);   // cyan absorbs R
        outc *= 1.0 - cov_m * float3(0.10, 0.95, 0.10);   // magenta absorbs G
        outc *= 1.0 - cov_y * float3(0.02, 0.05, 0.90);   // yellow absorbs B
        outc *= 1.0 - cov_k * float3(0.97, 0.97, 0.97);
    }

    const float4 result = float4(fx_srgb_eotf(saturate(outc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
