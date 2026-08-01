// Slit-scan: rows (or columns) sample progressively older
// frames from a past-frames ring buffer — the frame becomes a time
// gradient. The engine dispatches this kernel once per age band with the
// matching history frame bound; bands are disjoint, so the passes tile the
// output exactly once.
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
    float mode;      // 0 = rows map to time, 1 = columns
    float depth;     // frames of history in use
    float reverse;   // flip the time gradient
    uint pass_age;   // this dispatch's age band (engine-provided)
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
