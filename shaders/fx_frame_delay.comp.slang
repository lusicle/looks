// Frame Delay (docs/flow_canvas.md primitives): plain N-frame delay
// — the engine binds the right slice of the per-instance past-frames
// ring as the second input; the kernel just presents it. Composes with
// Blend / Motion Extract for echo and difference rigs.
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
    float frames;   // delay depth (engine-side ring pick; unread here)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D delayed;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float4 past = delayed.SampleLevel(uv, 0);

    const float4 wetted = lerp(base, past, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
