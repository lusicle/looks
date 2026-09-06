#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push {
    uint width; uint height; float wet; float opacity; uint seed; uint frame; float fps;
    float canvas_w; float canvas_h; uint history_valid;
    float af; float ae; float awb; float rate;
};
[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D previous;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy != 0)) return;
    float3 average = 0.0;
    float contrast = 0.0;
    float weight = 0.0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            float2 uv = (float2(x, y) + 0.5) / 8.0;
            float4 s = input.SampleLevel(uv, 0);
            float w = exp(-dot(uv - 0.5, uv - 0.5) * 4.0);
            average += s.rgb * w;
            weight += s.a * w;
            float2 dx = float2(2.0 / canvas_w, 0.0);
            contrast += abs(fx_luma(input.SampleLevel(uv + dx, 0).rgb) -
                            fx_luma(input.SampleLevel(uv - dx, 0).rgb)) * w;
        }
    }
    average /= max(weight, 0.001);
    contrast /= max(weight, 0.001);
    float t = float(frame) / max(fps, 1.0);
    float lock = saturate(contrast * 12.0);
    float4 target = float4(clamp(log2(0.18 / max(fx_luma(average), 0.002)), -4.0, 4.0),
        clamp(log2(max(average.g, 0.01) / max(average.r, 0.01)), -1.0, 1.0),
        clamp(log2(max(average.g, 0.01) / max(average.b, 0.01)), -1.0, 1.0),
        (1.0 - lock) * fx_value_noise1(seed, t * rate) * 9.0);
    if (weight <= 0.001) target = 0.0;
    float response = 1.0 - exp(-max(rate, 0.1) * 6.0 / max(fps, 1.0));
    output[uint2(0, 0)] = history_valid != 0 ?
        lerp(previous.SampleLevel(float2(0.5, 0.5), 0), target, response) : target;
}
