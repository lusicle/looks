// Flow smear (category 1): accumulate taps backward along the
// motion vector — moving regions streak, still regions stay sharp.
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
    float amount;      // smear length multiplier
    float falloff;     // 0 = uniform trail, 1 = fast decay
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D flow;    // block MVs in px, (w/16, h/16)
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 mv = flow.SampleLevel(uv, 0).rg * amount;

    float3 sum = base.rgb;
    float weight_sum = 1.0;
    [unroll]
    for (int i = 1; i <= 12; ++i)
    {
        const float t = float(i) / 12.0;
        const float w = 1.0 - saturate(falloff) * t;
        const float2 p = uv - mv * t / size;
        sum += input.SampleLevel(saturate(p), 0).rgb * w;
        weight_sum += w;
    }
    const float4 result = float4(sum / weight_sum, base.a);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
