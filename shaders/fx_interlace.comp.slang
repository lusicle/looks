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
    float shift;    // comb offset, px
    float darken;
    float mode;     // 0 comb and lines, 1 lines only
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

    const float field = (tid.y & 1u) != 0u ? 1.0 : -1.0;
    const float parity = (frame & 1u) != 0u ? 1.0 : -1.0;

    float2 suv = uv;
    if (mode < 0.5)
        suv.x = clamp(uv.x + field * parity * shift / size.x, 0.0, 1.0);
    float4 result = input.SampleLevel(suv, 0);
    if ((tid.y & 1u) != 0u)
        result.rgb *= 1.0 - saturate(darken);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
