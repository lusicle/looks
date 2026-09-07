#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push { uint width; uint height; };
[[vk::binding(0, 0)]] Sampler2D source;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 pixel = source.SampleLevel(uv, 0);
    output[tid.xy] = float4(fx_srgb_eotf(pixel.rgb) * pixel.a, pixel.a);
}
