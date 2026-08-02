// Linear light and uniform across the frame.

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
    float rate;    // hz
    float mode;    // 0 projector beat, 1 random gate, 2 color strobe
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    float gain;
    float3 tint = float3(1.0, 1.0, 1.0);
    if (mode < 0.5) {
        // Two shutter harmonics beating, as a blade does against mains.
        const float beat = 0.6 * sin(6.28318 * rate * t) +
                           0.4 * sin(6.28318 * rate * 0.37 * t + 1.3);
        gain = 1.0 - amount * 0.25 * (beat + 1.0);
    } else if (mode < 1.5) {
        const uint tick = uint(t * rate);
        const float g = fx_hash01(fx_hash3(seed, tick, 5u));
        gain = 1.0 - amount * 0.6 * g * g;
    } else {
        // Exposure holds here and chroma pulses instead.
        const uint tick = uint(t * rate);
        const float hue = fx_hash01(fx_hash3(seed, tick, 11u));
        tint = lerp(float3(1.0, 1.0, 1.0), fx_hue_to_rgb(hue),
                    saturate(amount));
        gain = 1.0;
    }

    const float4 result = float4(base.rgb * gain * tint, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
