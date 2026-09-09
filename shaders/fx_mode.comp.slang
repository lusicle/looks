#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push {
    uint width;
    uint height;
    float wet;
    float opacity;
    uint seed;
    uint frame;
    float fps;
    float tolerance;
    float samples;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D plate;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float4 base = input.Load(int3(tid.xy, 0));
    float4 result = plate.SampleLevel((float2(tid.xy) + 0.5) / float2(width, height), 0.0);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
