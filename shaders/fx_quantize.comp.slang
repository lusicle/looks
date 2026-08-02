// Display-referred: the kernel converts linear -> sRGB around its core.
// Dither mode 9 (RD stipple) reads a Gray-Scott sim the engine steps
// per instance and binds at binding 2; every other mode ignores it.

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
    float levels;
    float palette;
    float dither;
    float dither_amt;
    float boil_hz;     // 0 = static
    float scroll;
    float warp;
    float dissolve;    // 1 = fully quantized, 0 reveals the input
    float lock_mode;
    float pat_rotate;  // degrees
    float pat_scale;
    float pal_hue_a;
    float pal_hue_b;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D noise_lut;   // packed LUT, fx_common
[[vk::binding(2, 0)]] Sampler2D rd_state;    // Gray-Scott (A, B), mode 9
[[vk::binding(3, 0)]] Sampler2D flow;
[[vk::binding(4, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    float3 c = fx_srgb_oetf(saturate(base.rgb));

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint boil_tick = boil_hz > 0.01 ? uint(t * boil_hz) : 0u;
    const uint2 shift = uint2(fx_hash3(seed, boil_tick, 11u) & 127u,
                              fx_hash3(seed, boil_tick, 13u) & 127u);

    // Motion lock advects the pattern along the flow field so it rides
    // the content instead of the screen.
    float2 pf = float2(tid.xy);
    if (lock_mode >= 0.5) {
        const float2 mv = flow.SampleLevel(uv, 0).rg;
        pf -= mv;
    }
    if (abs(pat_rotate) > 0.01) {
        const float pr = pat_rotate * 0.01745329252;
        const float prc = cos(pr), prs = sin(pr);
        pf = float2(pf.x * prc - pf.y * prs, pf.x * prs + pf.y * prc);
    }
    // Scale floors at 1: one pixel is the finest possible dither
    // feature, and sub-1 strides alias the matrix onto a near-constant
    // threshold subset (stride 4 on bayer 8 samples only {0..3}/64).
    pf /= clamp(pat_scale, 1.0, 4.0);
    pf += float2(shift);
    pf += t * scroll * float2(1.0, 0.37);
    if (warp > 0.001) {
        pf += warp * 20.0 *
              float2(sin(pf.y * 0.043 + t * 0.9),
                     sin(pf.x * 0.037 - t * 0.7));
    }
    const uint2 p = uint2(int2(floor(pf)) & 0x7FFF);

    const int dm = int(dither + 0.5);
    float pattern = 0.5;
    if (dm == 1)
        pattern = (kFxBayer2[(p.y & 1) * 2 + (p.x & 1)] + 0.5) / 4.0;
    else if (dm == 2)
        pattern = (kFxBayer4[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
    else if (dm == 3)
        pattern = fx_bayer8(p) + 0.5 / 64.0;
    else if (dm == 4)
        pattern = fx_hash01(fx_hash4(seed, boil_tick, tid.x, tid.y));
    else if (dm == 5)
        pattern = fx_blue_noise(noise_lut, p);
    else if (dm == 6)
        pattern = fx_stbn(noise_lut, p, boil_hz > 0.01 ? boil_tick : 0u);
    else if (dm == 7) {
        // Two ordered patterns at drifting scale/angle, interfering.
        const float ang = t * 0.15;
        const float ca = cos(ang), sa = sin(ang);
        const float2 r1 = float2(pf.x * ca - pf.y * sa,
                                 pf.x * sa + pf.y * ca);
        const float2 r2 = pf * 1.09;
        const float m = fx_bayer8(uint2(int2(floor(r1)) & 0x7FFF)) +
                        fx_bayer8(uint2(int2(floor(r2)) & 0x7FFF));
        pattern = frac(m);
    }
    else if (dm == 9) {
        // The sim's B concentration: dots land near 0.35, field at 0.
        pattern = saturate(rd_state.SampleLevel(uv, 0).g * 2.5);
    }
    else if (dm == 10) {
        const float2 d = pf - float2(float(width), float(height)) * 0.5;
        pattern =
            frac(length(d) * 0.125 - atan2(d.y, d.x) * 0.15915494);
    }
    else if (dm == 11) {
        const float2 d = pf - float2(float(width), float(height)) * 0.5;
        pattern = frac(length(d) * 0.125);
    }
    else if (dm == 12) {
        const float2 f = frac(pf * 0.125) - 0.5;
        pattern = saturate((abs(f.x) + abs(f.y)) * 1.4);
    }
    else if (dm == 13)
        pattern = fx_cluster_dot(pf);
    else if (dm == 14) {
        pattern = (float(p.y & 3) + 0.5) * 0.25;
    }
    else if (dm == 15)
        pattern = ((p.x ^ p.y) & 1u) != 0u ? 0.75 : 0.25;
    else if (dm == 16)
        pattern = fx_ign(p);
    float threshold = (pattern - 0.5) * saturate(dither_amt);
    if (dm == 0 || dm == 8) threshold = 0.0;

    const float cycle_rate = boil_hz > 0.01 ? boil_hz : 2.0;
    const uint cycle_step = dm == 8 ? uint(t * cycle_rate) : 0u;

    const int pal = int(palette + 0.5);
    const float n = max(levels, 2.0);
    // Level picks happen in LINEAR light so dithered fields average back
    // to the source brightness; code-value dithering would lift every
    // dark region on display. The fixed palettes below deliberately keep
    // their stylized encoded-domain snap instead.
    const float t01 = clamp(0.5 + threshold, 0.0, 1.0);
    float3 result;
    if (pal == 0)
    {
        float3 idx = fx_dither_levels(base.rgb, n, t01);
        if (cycle_step != 0u)
            idx = float3(fmod(idx.r + float(cycle_step), n),
                         fmod(idx.g + float(cycle_step), n),
                         fmod(idx.b + float(cycle_step), n));
        result = idx / (n - 1.0);
    }
    else if (pal == 1)
    {
        const float y = fx_luma(saturate(base.rgb));
        float idx = fx_dither_levels(float3(y, y, y), n, t01).x;
        if (cycle_step != 0u) idx = fmod(idx + float(cycle_step), n);
        const float q = idx / (n - 1.0);
        result = float3(q, q, q);
    }
    else if (pal == 2)
    {
        // DMG greens, darkest to lightest.
        float l = saturate(fx_luma(c) + threshold * 0.34);
        if (cycle_step != 0u) l = frac(l + float(cycle_step & 3u) * 0.25);
        result = fx_nearest_of4(float3(l, l, l),
                                float3(0.059, 0.220, 0.059),
                                float3(0.188, 0.384, 0.188),
                                float3(0.545, 0.675, 0.059),
                                float3(0.608, 0.737, 0.059));
    }
    else if (pal == 3)
    {
        // CGA palette 1.
        float3 dithered = saturate(c + threshold * 0.5);
        if (cycle_step != 0u)
            dithered = frac(dithered + float(cycle_step & 3u) * 0.25);
        result = fx_nearest_of4(dithered,
                                float3(0.0, 0.0, 0.0),
                                float3(0.333, 1.0, 1.0),
                                float3(1.0, 0.333, 1.0),
                                float3(1.0, 1.0, 1.0));
    }
    else if (pal == 4)
    {
        float3 dithered = saturate(c + threshold * 0.5);
        if (cycle_step != 0u)
            dithered = frac(dithered + float(cycle_step & 3u) * 0.25);
        result = fx_nes_nearest(dithered);
    }
    else if (pal == 5)
    {
        // The 8 saturated teletext colors: each channel snaps to 0 or 1.
        float3 dithered = saturate(c + threshold * 0.5);
        if (cycle_step != 0u)
            dithered = frac(dithered + float(cycle_step & 3u) * 0.25);
        result = round(dithered);
    }
    else
    {
        // Duotone: black -> hue A -> hue B -> white by luma.
        float l = saturate(fx_luma(c) + threshold * 0.34);
        if (cycle_step != 0u) l = frac(l + float(cycle_step & 3u) * 0.25);
        const float3 ha = fx_hue_to_rgb(pal_hue_a);
        const float3 hb = fx_hue_to_rgb(pal_hue_b);
        result = fx_nearest_of4(float3(l, l, l),
                                float3(0.02, 0.02, 0.02),
                                ha * 0.45,
                                hb * 0.85,
                                float3(0.98, 0.98, 0.98));
    }

    if (dissolve < 0.999) {
        // Patternless modes (none, level cycle) gate on a hash instead.
        const float gate = (dm == 0 || dm == 8)
            ? fx_hash01(fx_hash4(seed, 3u, tid.x, tid.y))
            : pattern;
        result = gate <= dissolve ? result : c;
    }

    const float4 quantized = float4(fx_srgb_eotf(result), base.a);
    const float4 wetted = lerp(base, quantized, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
