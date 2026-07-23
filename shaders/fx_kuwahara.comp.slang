// Kuwahara (spec §6.6): classic 4-quadrant filter — each pixel takes the
// mean of whichever surrounding quadrant has the least variance. Flattens
// texture into paint daubs while keeping edges crisp. Encoded space.
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
    float radius;   // px, 1..6
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const int r = clamp(int(radius + 0.5), 1, 6);
    float3 best_mean = 0.0;
    float best_var = 1.0e9;

    [unroll]
    for (int q = 0; q < 4; ++q) {
        const int sx = (q & 1) == 0 ? -1 : 0;   // quadrant x range start
        const int sy = (q & 2) == 0 ? -1 : 0;
        float3 sum = 0.0;
        float sum_l = 0.0, sum_l2 = 0.0;
        float n = 0.0;
        for (int j = 0; j <= 6; ++j) {
            if (j > r) break;
            for (int i = 0; i <= 6; ++i) {
                if (i > r) break;
                const float ox = sx == 0 ? float(i) : -float(i);
                const float oy = sy == 0 ? float(j) : -float(j);
                const float3 s = fx_srgb_oetf(saturate(
                    input.SampleLevel(uv + float2(ox, oy) / size, 0).rgb));
                const float l = fx_luma(s);
                sum += s;
                sum_l += l;
                sum_l2 += l * l;
                n += 1.0;
            }
        }
        const float mean_l = sum_l / n;
        const float variance = sum_l2 / n - mean_l * mean_l;
        if (variance < best_var) {
            best_var = variance;
            best_mean = sum / n;
        }
    }

    const float4 result = float4(fx_srgb_eotf(saturate(best_mean)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
