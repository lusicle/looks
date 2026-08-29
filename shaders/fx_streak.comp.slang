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
    float length_px;
    float angle;
    float decay;       // per tap
    float threshold;
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

    const float2 dir = float2(cos(angle), sin(angle));
    const bool glow_mode = threshold > 0.001;

    float3 acc = base.rgb;
    float acc_a = base.a;
    float wsum = 1.0;
    float3 add = float3(0.0, 0.0, 0.0);
    float add_a = 0.0;
    float w = 1.0;
    [unroll]
    for (int i = 1; i <= 24; ++i) {
        w *= decay;
        const float d = float(i) / 24.0 * length_px;
        const float2 p = clamp(uv - dir * d / size, 0.0, 1.0);
        const float4 c = input.SampleLevel(p, 0);
        if (glow_mode) {
            add += max(c.rgb - threshold, 0.0) * w;
            add_a = max(add_a,
                        c.a * w * saturate(fx_luma(max(c.rgb - threshold,
                                                       0.0))));
        } else {
            acc += c.rgb * w;
            acc_a += c.a * w;
            wsum += w;
        }
    }

    const float3 outc =
        glow_mode ? base.rgb + add * (3.0 / 24.0) : acc / wsum;
    const float out_a =
        glow_mode ? max(base.a, add_a) : acc_a / wsum;
    const float4 result = float4(outc, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
