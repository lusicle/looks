// Dither (docs/flow_canvas.md v5.5 primitives): the Quantizer's ordered-
// pattern threshold engine (spec §6.2), standalone. Patterns: 0-2 Bayer
// 2/4/8, 3 white noise, 4 blue noise, 5 STBN (temporal slices on the
// boil clock), 6 moiré, 7 spiral, 8 radial rings, 9 diamond cluster.
// Shared pattern-transform controls: boil / scroll / rotate / scale /
// motion lock (flow advection). Levels are encoded-uniform but the pick
// is made in LINEAR light (fx_dither_levels) so brightness is preserved.
//
// Binding 1 is the packed dither LUT (see fx_common.slang); binding 2 the
// flow field (bound as the LUT dummy unless motion lock wired it).
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
    float levels;      // quantization steps
    float pattern_m;   // pattern mode, see header
    float amount;      // 0..1 threshold strength
    float boil_hz;     // pattern re-seed rate; 0 = static
    float scroll;      // pattern drift, px/s
    float pat_rotate;  // pattern transform: rotation, degrees
    float pat_scale;   // pattern transform: scale
    float lock_mode;   // 0 screen-locked, 1 motion-locked
    float mode;        // 0 per-channel rgb, 1 grayscale
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D noise_lut;
[[vk::binding(2, 0)]] Sampler2D flow;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    // Boil: the pattern re-seeds/offsets at boil_hz.
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint boil_tick = boil_hz > 0.01 ? uint(t * boil_hz) : 0u;
    const uint2 shift = uint2(fx_hash3(seed, boil_tick, 11u) & 127u,
                              fx_hash3(seed, boil_tick, 13u) & 127u);

    // Pattern coordinates (spec §6.2 shared controls — same construction
    // as the Quantizer's dither leg).
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
    const uint2 p = uint2(int2(floor(pf)) & 0x7FFF);

    const int dm = int(pattern_m + 0.5);
    float pattern = 0.5;
    if (dm == 0)
        pattern = (kFxBayer2[(p.y & 1) * 2 + (p.x & 1)] + 0.5) / 4.0;
    else if (dm == 1)
        pattern = (kFxBayer4[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
    else if (dm == 2)
        pattern = fx_bayer8(p) + 0.5 / 64.0;
    else if (dm == 3)
        pattern = fx_hash01(fx_hash4(seed, boil_tick, tid.x, tid.y));
    else if (dm == 4)
        pattern = fx_blue_noise(noise_lut, p);
    else if (dm == 5)
        pattern = fx_stbn(noise_lut, p, boil_hz > 0.01 ? boil_tick : 0u);
    else if (dm == 6) {
        const float ang = t * 0.15;
        const float ca = cos(ang), sa = sin(ang);
        const float2 r1 = float2(pf.x * ca - pf.y * sa,
                                 pf.x * sa + pf.y * ca);
        const float2 r2 = pf * 1.09;
        const float m = fx_bayer8(uint2(int2(floor(r1)) & 0x7FFF)) +
                        fx_bayer8(uint2(int2(floor(r2)) & 0x7FFF));
        pattern = frac(m);
    }
    else if (dm == 7) {
        const float2 d = pf - float2(float(width), float(height)) * 0.5;
        pattern =
            frac(length(d) * 0.125 - atan2(d.y, d.x) * 0.15915494);
    }
    else if (dm == 8) {
        const float2 d = pf - float2(float(width), float(height)) * 0.5;
        pattern = frac(length(d) * 0.125);
    }
    else if (dm == 9) {
        const float2 f = frac(pf * 0.125) - 0.5;
        pattern = saturate((abs(f.x) + abs(f.y)) * 1.4);
    }
    // Ordered threshold in [0, 1]; amount narrows it toward the plain
    // midpoint round.
    const float t01 =
        clamp(0.5 + (pattern - 0.5) * saturate(amount), 0.0, 1.0);

    // Level pick happens in LINEAR light (fx_dither_levels, v5.6a) so
    // the dithered field averages back to the source brightness — code-
    // value dithering lifts every dark region on display.
    const float n = max(levels, 2.0);
    float3 result;
    if (mode >= 0.5) {
        const float y = fx_luma(saturate(base.rgb));
        const float q =
            fx_dither_levels(float3(y, y, y), n, t01).x / (n - 1.0);
        result = float3(q, q, q);
    } else {
        result = fx_dither_levels(base.rgb, n, t01) / (n - 1.0);
    }

    const float4 dithered = float4(fx_srgb_eotf(result), base.a);
    const float4 wetted = lerp(base, dithered, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
