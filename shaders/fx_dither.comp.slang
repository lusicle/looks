// Binding 2 is a dummy unless motion lock wires a real flow field.

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
    float pattern_m;
    float amount;
    float boil_hz;     // hz
    float scroll;      // px/s
    float pat_rotate;  // degrees
    float pat_scale;
    float lock_mode;
    float mode;
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint boil_tick = boil_hz > 0.01 ? uint(t * boil_hz) : 0u;
    const float2 mv = lock_mode >= 0.5 ? flow.SampleLevel(uv, 0).rg
                                       : float2(0.0, 0.0);
    const float2 pf = fx_pattern_coords(
        tid.xy, mv, pat_rotate, pat_scale,
        fx_pattern_shift(seed, boil_tick), t, scroll, 0.0);

    const float pattern = fx_ordered_pattern(
        int(pattern_m + 0.5), pf, tid.xy, seed, boil_tick, t,
        float2(float(width), float(height)), noise_lut);
    const float t01 =
        clamp(0.5 + (pattern - 0.5) * saturate(amount), 0.0, 1.0);

    // Quantize in linear light so the dither averages to source brightness.
    const float n = max(levels, 2.0);
    float3 result;
    if (mode >= 0.5) {
        const float y = fx_luma(saturate(fx_straight(base)));
        const float q =
            fx_dither_levels(float3(y, y, y), n, t01).x / (n - 1.0);
        result = float3(q, q, q);
    } else {
        result = fx_dither_levels(fx_straight(base), n, t01) / (n - 1.0);
    }

    const float4 dithered = fx_from_encoded(result, base.a);
    const float4 wetted = lerp(base, dithered, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
