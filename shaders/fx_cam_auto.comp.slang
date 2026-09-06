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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float af;
    float ae;
    float awb;
    float rate;   // hz
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D meter;
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float r = max(rate, 0.05);

    const float4 measured = meter.SampleLevel(float2(0.5, 0.5), 0);
    const float defocus = af * measured.a;

    float3 col = base.rgb;
    float col_a = base.a;
    if (defocus > 0.3) {
        float3 acc = col;
        float acc_a = col_a;
        float wsum = 1.0;
        [unroll]
        for (int i = 0; i < 12; ++i) {
            const float fi = (float(i) + 0.5) / 12.0;
            const float a = float(i) * 2.39996;
            const float rr = sqrt(fi) * defocus;
            const float2 p =
                clamp(uv + float2(cos(a), sin(a)) * rr / float2(canvas_w, canvas_h), 0.0, 1.0);
            const float4 s = input.SampleLevel(p, 0);
            acc += s.rgb;
            acc_a += s.a;
            wsum += 1.0;
        }
        col = acc / wsum;
        col_a = acc_a / wsum;
    }

    const float gain = exp2(measured.r * ae);
    col *= gain;
    const float floor_amt = ae * max(gain - 1.0, 0.0) * 0.35;
    if (floor_amt > 0.001) {
        const float n =
            fx_hash01(fx_hash4(seed, frame, tid.x, tid.y)) - 0.5;
        const float shadow = 1.0 - saturate(fx_luma(fx_straight(float4(col, col_a))) * 3.0);
        col += n * floor_amt * shadow * col_a;
    }

    col *= exp2(float3(measured.g, 0.0, measured.b) * awb);

    const float4 result = float4(max(col, 0.0), col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
