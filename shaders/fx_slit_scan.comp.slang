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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float mode;
    float depth;
    float reverse;
    uint pass_age;   // the engine appends this age band word
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D history;   // frame at age == pass_age
[[vk::binding(2, 0)]] Sampler2D history_next;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint n = uint(max(depth, 2.0));
    const bool rows = mode < 0.5;
    const uint extent = rows ? height : width;
    const uint start = extent * pass_age / n;
    const uint end = extent * (pass_age + 1u) / n;
    const uint local = rows ? tid.y : tid.x;
    if (local >= end - start) return;
    const uint2 pixel = tid.xy + (rows ? uint2(0, start) : uint2(start, 0));
    if (pixel.x >= width || pixel.y >= height)
        return;
    const float2 uv = (float2(pixel) + 0.5) / float2(width, height);

    const float4 base = input.SampleLevel(uv, 0);
    const float4 result = lerp(history.SampleLevel(uv, 0), history_next.SampleLevel(uv, 0),
                              (float(local) + 0.5) / float(end - start));

    const float4 wetted = lerp(base, result, saturate(wet));
    output[pixel] = lerp(base, wetted, saturate(opacity));
}
