// Display-referred: the kernel converts linear -> sRGB around its core.
// Dither mode 9 (RD stipple) reads a Gray-Scott sim the engine steps
// per instance and binds at binding 2; every other mode ignores it.

#include "fx_pattern.slang"

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
    const float2 mv = lock_mode >= 0.5 ? flow.SampleLevel(uv, 0).rg
                                       : float2(0.0, 0.0);
    const float2 pf = fx_pattern_coords(
        tid.xy, mv, pat_rotate, pat_scale,
        fx_pattern_shift(seed, boil_tick), t, scroll, warp);

    // Selector: 0 none, 1..7 patterns, 8 level cycle, 9 RD stipple,
    // 10..16 patterns again - the shared engine ids skip the two
    // specials, hence the -1 / -3 remap.
    const int dm = int(dither + 0.5);
    float pattern = 0.5;
    if (dm == 9) {
        // The sim's B concentration: dots land near 0.35, field at 0.
        pattern = saturate(rd_state.SampleLevel(uv, 0).g * 2.5);
    } else if (dm >= 1 && dm != 8) {
        pattern = fx_ordered_pattern(
            dm >= 10 ? dm - 3 : dm - 1, pf, tid.xy, seed, boil_tick, t,
            float2(float(width), float(height)), noise_lut);
    }
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
    else
    {
        // Fixed palettes: luma palettes (DMG, duotone) dither the luma,
        // color palettes dither the color, then the shared snap.
        float l = saturate(fx_luma(c) + threshold * 0.34);
        float3 dithered = saturate(c + threshold * 0.5);
        if (cycle_step != 0u) {
            l = frac(l + float(cycle_step & 3u) * 0.25);
            dithered = frac(dithered + float(cycle_step & 3u) * 0.25);
        }
        result = fx_palette_snap(pal - 2, dithered, l,
                                 pal_hue_a, pal_hue_b);
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
