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
    float position;   // normalized, the frozen row or column
    float mode;       // 0 smear down, 1 up, 2 right, 3 left
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

    const uint m = uint(mode + 0.5);
    float2 suv = uv;
    if (m == 0u && uv.y > position) suv.y = position;
    else if (m == 1u && uv.y < position) suv.y = position;
    else if (m == 2u && uv.x > position) suv.x = position;
    else if (m == 3u && uv.x < position) suv.x = position;
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
