// The bright pass works in linear space.

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
    float amount;      // the composite pass uses this, not the shader
    float radius;      // the blur passes use this, not the shader
    float threshold;
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
    const float4 src = input.SampleLevel(uv, 0);
    const float3 rgb = src.rgb;

    const float l = fx_luma(rgb);
    const float knee = max(threshold, 1.0e-3);
    float gain = smoothstep(knee * 0.5, knee * 1.5, l);
    if (int(mode + 0.5) == 0)
        gain = max(gain, 0.25);
    // Alpha is the source coverage gated by how much of it glows.
    output[tid.xy] = float4(rgb * gain, src.a * gain);
}
