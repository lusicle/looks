// NR mush (spec §6.5): denoiser over-smoothing — a bilateral blur whose
// range weight collapses detail into watercolor patches while edges with
// enough contrast survive. Encoded space (denoisers run on signal).
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
    float amount;   // how aggressively detail collapses
    float radius;   // px
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

    const float3 center = fx_srgb_oetf(saturate(base.rgb));
    // Wide range sigma = more mush.
    const float sigma = lerp(0.08, 0.45, saturate(amount));
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float3 acc = center;
    float wsum = 1.0;
    [unroll]
    for (int j = -2; j <= 2; ++j) {
        [unroll]
        for (int i = -2; i <= 2; ++i) {
            if (i == 0 && j == 0) continue;
            const float2 o = float2(i, j) * (radius * 0.5) / size;
            const float3 s =
                fx_srgb_oetf(saturate(input.SampleLevel(uv + o, 0).rgb));
            const float3 d = s - center;
            const float w = exp(-dot(d, d) * inv2s2);
            acc += s * w;
            wsum += w;
        }
    }
    const float3 mush = acc / wsum;

    const float4 result = float4(fx_srgb_eotf(saturate(mush)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
