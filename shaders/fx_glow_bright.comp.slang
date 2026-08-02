// Glow pass 1: bright pass in linear space. Downstream is blur H, blur V,
// then composite.

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
    float amount;      // consumed by the composite pass, unused here
    float radius;      // consumed by the blur passes, unused here
    float threshold;
    float mode;        // 0 pro-mist, 1 halation, 2 ccd smear
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
    const float3 rgb = input.SampleLevel(uv, 0).rgb;

    const float l = fx_luma(rgb);
    // Pro-mist floors the gain, so it diffuses everything a little.
    const float knee = max(threshold, 1.0e-3);
    float gain = smoothstep(knee * 0.5, knee * 1.5, l);
    if (int(mode + 0.5) == 0)
        gain = max(gain, 0.25);
    output[tid.xy] = float4(rgb * gain, 1.0);
}
