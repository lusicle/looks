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
    float af;     // focus hunt
    float ae;     // exposure pump
    float awb;    // white-balance drift
    float rate;   // hz
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float r = max(rate, 0.05);

    // Squaring the noise keeps focus sharp most of the time, then lunges.
    const float hunt = fx_value_noise1(seed, t * r * 1.3);
    const float defocus = af * 9.0 * hunt * hunt;

    float3 col = base.rgb;
    if (defocus > 0.3) {
        float3 acc = col;
        float wsum = 1.0;
        [unroll]
        for (int i = 0; i < 12; ++i) {
            const float fi = (float(i) + 0.5) / 12.0;
            const float a = float(i) * 2.39996;
            const float rr = sqrt(fi) * defocus;
            const float2 p =
                clamp(uv + float2(cos(a), sin(a)) * rr / size, 0.0, 1.0);
            acc += input.SampleLevel(p, 0).rgb;
            wsum += 1.0;
        }
        col = acc / wsum;
    }

    const float target =
        (fx_value_noise1(seed ^ 0x0AEAu, t * r * 0.8) - 0.5) * 2.0;
    const float gain = 1.0 + target * ae * 0.55;
    col *= gain;
    // Gain drags the noise floor with it, so shadows crawl on a pump up.
    const float floor_amt = ae * max(gain - 1.0, 0.0) * 0.35;
    if (floor_amt > 0.001) {
        const float n =
            fx_hash01(fx_hash4(seed, frame, tid.x, tid.y)) - 0.5;
        const float shadow = 1.0 - saturate(fx_luma(col) * 3.0);
        col += n * floor_amt * shadow;
    }

    const float temp =
        (fx_value_noise1(seed ^ 0xA3Bu, t * r * 0.5) - 0.5) * 2.0 * awb;
    col *= float3(1.0 + temp * 0.28, 1.0 + temp * 0.04, 1.0 - temp * 0.30);

    const float4 result = float4(max(col, 0.0), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
