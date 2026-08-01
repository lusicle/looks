// Time displacement (adjacent): each pixel plays back from a
// different moment — its delay driven by the luma of a map (self, or a
// mask's grayscale wired as the third input). The classic "bright is now,
// dark is the past" video-synth effect. Same engine mechanism as
// slit-scan: one dispatch per age band with that band's history bound;
// bands partition the pixels, so the passes tile the output exactly once.
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
    float range;      // frames of history in use
    float mode;       // 0 dark lags, 1 bright lags, 2 map input lags-dark
    float band_soft;  // hash dither on the band edges
    uint pass_age;    // this dispatch's age band (engine-provided)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D history;   // frame at age == pass_age
[[vk::binding(2, 0)]] Sampler2D map;       // delay map (self when unwired)
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);

    const uint m = uint(mode + 0.5);
    const float v = m == 2u
        ? fx_luma(map.SampleLevel(uv, 0).rgb)
        : fx_luma(input.SampleLevel(uv, 0).rgb);
    const float lag = m == 1u ? saturate(v) : 1.0 - saturate(v);

    const float n = clamp(range, 2.0, 16.0);
    const float dither =
        (fx_hash01(fx_hash4(seed, tid.x, tid.y, 9u)) - 0.5) * band_soft *
        1.5;
    const uint band = uint(clamp(lag * (n - 1.0) + dither + 0.5, 0.0,
                                 n - 1.0));
    if (band != pass_age)
        return;

    const float4 base = input.SampleLevel(uv, 0);
    const float4 result = history.SampleLevel(uv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
