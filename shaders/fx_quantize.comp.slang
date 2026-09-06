// Work in display-referred space, not in linear light.
// Binding 2 holds the Gray-Scott sim. Only dither mode 9 reads it.

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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float levels;
    float palette;
    float dither;
    float dither_amt;
    float boil_hz;     // hz
    float scroll;
    float warp;
    float dissolve;
    float lock_mode;
    float pat_rotate;  // degrees
    float pat_scale;
    float pal_hue_a;
    float pal_hue_b;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D noise_lut;   // the packed LUT
[[vk::binding(2, 0)]] Sampler2D rd_state;    // (A, B) state
[[vk::binding(3, 0)]] Sampler2D flow;
[[vk::binding(4, 0)]] RWTexture2D<float4> output;

float4 quantized_sample(float2 uv)
{
    const float4 base = input.SampleLevel(uv, 0);

    const float3 straight = saturate(fx_straight(base));
    float3 c = fx_srgb_oetf(straight);

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint boil_tick = boil_hz > 0.01 ? uint(t * boil_hz) : 0u;
    const float2 mv = lock_mode >= 0.5 ? flow.SampleLevel(uv, 0).rg * float2(canvas_w / width, canvas_h / height)
                                       : float2(0.0, 0.0);
    const float2 pf = fx_pattern_coords(
        uint2(uv * float2(canvas_w, canvas_h)), mv, pat_rotate, pat_scale,
        fx_pattern_shift(seed, boil_tick), t, scroll, warp);

    // The shared ids skip two specials, so the remap uses -1 and -3.
    const int dm = int(dither + 0.5);
    float pattern = 0.5;
    if (dm == 9) {
        // The B concentration is near 0.35 on dots and 0 in the field.
        pattern = saturate(rd_state.SampleLevel(uv, 0).g * 2.5);
    } else if (dm >= 1 && dm != 8) {
        pattern = fx_ordered_pattern(
            dm >= 10 ? dm - 3 : dm - 1, pf, uint2(uv * float2(canvas_w, canvas_h)), seed, boil_tick, t,
            float2(canvas_w, canvas_h), noise_lut);
    }
    float threshold = (pattern - 0.5) * saturate(dither_amt);
    if (dm == 0 || dm == 8) threshold = 0.0;

    const float cycle_rate = boil_hz > 0.01 ? boil_hz : 2.0;
    const uint cycle_step = dm == 8 ? uint(t * cycle_rate) : 0u;

    const int pal = int(palette + 0.5);
    const float n = max(levels, 2.0);
    // The level picks use linear light so the dither averages back.
    // The fixed palettes keep their encoded-domain snap on purpose.
    const float t01 = clamp(0.5 + threshold, 0.0, 1.0);
    float3 result;
    if (pal == 0)
    {
        float3 idx = fx_dither_levels(straight, n, t01);
        if (cycle_step != 0u)
            idx = float3(fmod(idx.r + float(cycle_step), n),
                         fmod(idx.g + float(cycle_step), n),
                         fmod(idx.b + float(cycle_step), n));
        result = idx / (n - 1.0);
    }
    else if (pal == 1)
    {
        const float y = fx_luma(straight);
        float idx = fx_dither_levels(float3(y, y, y), n, t01).x;
        if (cycle_step != 0u) idx = fmod(idx + float(cycle_step), n);
        const float q = idx / (n - 1.0);
        result = float3(q, q, q);
    }
    else
    {
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
        const float gate = (dm == 0 || dm == 8)
            ? fx_hash01(fx_hash4(seed, 3u, uint(uv.x * canvas_w), uint(uv.y * canvas_h)))
            : pattern;
        result = gate <= dissolve ? result : c;
    }

    return float4(fx_srgb_eotf(result) * base.a, base.a);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 size = float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) / size;
    float4 base = input.SampleLevel(uv, 0);
    uint2 taps = uint2(clamp(ceil(float2(canvas_w, canvas_h) / size), 1.0, 4.0));
    if (dither < 0.5 && dissolve >= 0.999) taps = uint2(1u, 1u);
    float4 result = 0.0;
    for (uint y = 0u; y < taps.y; ++y)
        for (uint x = 0u; x < taps.x; ++x)
            result += quantized_sample((float2(tid.xy) + (float2(x, y) + 0.5) / float2(taps)) / size);
    result /= float(taps.x * taps.y);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
