// Stutter / beat-repeat: while armed, the engine replays the
// last N recorded frames cyclically (the ring frame arrives on binding 1);
// unarmed it records and passes through. Route a beat trigger onto the
// "armed" param and the picture beat-repeats with the music.
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
    float frames;   // ring length (engine-side)
    float armed;    // >= 0.5 replays
    float rate;     // replay speed (engine-side)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D ring_frame;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float4 result =
        armed >= 0.5 ? ring_frame.SampleLevel(uv, 0) : base;
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
