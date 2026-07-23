// Frame-rate / shutter sim (spec §6.1): the engine refreshes the held
// frame only when the target-fps tick advances, so 30 fps footage judders
// like 16 mm. Shutter blend mixes the live frame back in — a cheap stand-in
// for 180° accumulation smear.
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

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
    float hold_fps;
    float blend;    // 0 = pure hold (judder), 1 = live
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D held;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float4 kept = held.SampleLevel(uv, 0);

    const float4 result = lerp(kept, base, saturate(blend) * 0.5);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
