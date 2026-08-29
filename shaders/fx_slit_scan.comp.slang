// The engine runs one dispatch for each age band.

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
    float mode;
    float depth;
    float reverse;
    uint pass_age;   // the engine appends this age band word
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D history;   // frame at age == pass_age
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);

    const float n = max(depth, 2.0);
    float v = mode < 0.5 ? uv.y : uv.x;
    if (reverse >= 0.5) v = 1.0 - v;
    const uint band = min(uint(v * n), uint(n) - 1u);
    if (band != pass_age)
        return;

    const float4 base = input.SampleLevel(uv, 0);
    const float4 result = history.SampleLevel(uv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
