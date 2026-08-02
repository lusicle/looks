// Glow pass 4: composites the blurred bright field over the dry input.

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
    float amount;
    float radius;
    float threshold;
    float mode;        // 0 pro-mist, 1 halation, 2 ccd smear
    float smear;       // ccd mode only
};

[[vk::binding(0, 0)]] Sampler2D dry;
[[vk::binding(1, 0)]] Sampler2D blurred;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = dry.SampleLevel(uv, 0);
    const float3 bloom = blurred.SampleLevel(uv, 0).rgb;

    float3 tint = float3(1.0, 1.0, 1.0);
    const int m = int(mode + 0.5);
    if (m == 1)
        tint = float3(1.0, 0.38, 0.16);
    else if (m == 2)
        tint = float3(0.85, 0.95, 1.1);

    float3 lit = base.rgb + bloom * tint * amount;

    // Samples the DRY input down the whole column, since a readout smear
    // is full-height and near-constant rather than a local bloom.
    if (m == 2 && smear > 0.001) {
        const float clip_at = max(threshold, 0.55);
        float3 col_energy = float3(0.0, 0.0, 0.0);
        [unroll]
        for (int s = 0; s < 24; ++s) {
            const float sy = (float(s) + 0.5) / 24.0;
            const float3 c = dry.SampleLevel(float2(uv.x, sy), 0).rgb;
            col_energy += max(c - clip_at, 0.0);
        }
        col_energy *= (1.0 / 24.0);
        lit += col_energy * tint * smear * 2.5;
    }
    const float4 result = float4(lit, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
