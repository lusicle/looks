// Slice shuffle: the frame splits into horizontal bands; a
// seeded per-band coin decides which bands displace sideways, re-rolled at
// `rate` Hz on the timeline clock. Displaced bands wrap around — the
// classic corrupted-scan look. Deterministic: (seed, step, band) hash.
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
    float slices;   // band count
    float amount;   // max displacement, px
    float rate;     // reshuffle rate, hz (0 = static)
    float prob;     // chance a band displaces
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

    const float n = clamp(round(slices), 2.0, 128.0);
    const uint band = uint(uv.y * n);
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint step = rate > 0.0 ? uint(t * rate) : 0u;

    const uint h = fx_hash4(seed, step, band, 0u);
    float ox = 0.0;
    if (fx_hash01(h) < prob)
        ox = (fx_hash01(fx_hash(h ^ 0xB5297A4Du)) * 2.0 - 1.0) * amount;

    float sx = uv.x + ox / size.x;
    sx -= floor(sx);   // wrap
    const float4 result = input.SampleLevel(float2(sx, uv.y), 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
