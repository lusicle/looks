// Keep the math in linear light.

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
    float amount;
    float rate;    // hz
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float begin = float(frame) * rate / max(fps, 1.0);
    const float span = max(rate / max(fps, 1.0), 1.0e-5);
    const float end = begin + span;
    float3 exposure = 0.0;
    if (mode < 0.5) {
        float a = floor(begin) * 0.5 + min(frac(begin), 0.5);
        float b = floor(end) * 0.5 + min(frac(end), 0.5);
        exposure = float3(2.0, 2.0, 2.0) * (b - a) / span;
    } else {
        for (uint tick = uint(floor(begin)); tick < uint(ceil(end)); ++tick) {
            float weight = max(min(end, float(tick + 1u)) - max(begin, float(tick)), 0.0) / span;
            float3 gain;
            if (mode < 1.5) {
                float g = 0.4 + 1.2 * fx_hash01(fx_hash3(seed, tick, 5u));
                gain = float3(g, g, g);
            } else {
                gain = fx_hue_to_rgb(fx_hash01(fx_hash3(seed, tick, 11u)));
                gain /= max(fx_luma(gain), 0.05);
            }
            exposure += gain * weight;
        }
    }
    const float4 result = float4(base.rgb * lerp(float3(1.0, 1.0, 1.0), exposure, saturate(amount)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
