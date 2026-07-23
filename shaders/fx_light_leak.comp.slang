// Light leaks (spec §6.3): warm blobs of fogging light breathing in from
// the frame edge — additive in LINEAR light, which is what makes leaks
// glow instead of paint. Position/intensity drift on seeded value noise.
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
    float amount;
    float hue;      // 0 = orange-red, 1 = magenta
    float drift;    // breathing rate, hz
    float burn;     // film-burn reel end (spec §6.3): 0 = off
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

    const float t = (fps > 0.0 ? float(frame) / fps : 0.0) * max(drift, 1.0e-3);

    // Two blobs anchored off the right edge, drifting vertically; a soft
    // band hugging the edge itself.
    float leak = 0.0;
    for (uint i = 0u; i < 2u; ++i) {
        const float cy = fx_value_noise1(seed + i * 17u, t + float(i) * 3.7);
        const float cx = 1.05 + 0.1 * fx_value_noise1(seed + i * 31u, t * 0.7);
        const float2 d = (uv - float2(cx, cy)) * float2(1.0, size.y / size.x);
        const float r = length(d);
        const float breathe =
            0.6 + 0.4 * fx_value_noise1(seed + i * 53u, t * 1.3);
        leak += breathe * exp(-r * r * 9.0);
    }
    const float edge = exp(-(1.0 - uv.x) * 5.0) * 0.5;
    leak = (leak + edge) * amount;

    const float3 warm = float3(1.0, 0.32, 0.08);
    const float3 magenta = float3(1.0, 0.12, 0.55);
    const float3 tint = lerp(warm, magenta, saturate(hue));

    float3 result_rgb = base.rgb + tint * leak;

    // Film-burn reel end (spec §6.3): a flickering burn-through creeping
    // in from the corners — orange fringe around a blown white core, the
    // emulsion melting look. Fully seeded/frame-driven.
    if (burn > 0.001) {
        const float tb = fps > 0.0 ? float(frame) / fps : 0.0;
        const float flicker =
            0.55 + 0.45 * fx_value_noise1(seed ^ 0xB00Bu, tb * 13.0);
        // Ragged radius from two corners, wobbling on value noise.
        const float2 corner = float2(1.0, 1.0);
        const float2 dc = (uv - corner) * float2(size.x / size.y, 1.0);
        const float ang = atan2(dc.y, dc.x);
        const float rag =
            0.35 * fx_value_noise1(seed ^ 0xF1AEu, ang * 2.2 + tb * 2.0);
        const float reach = burn * flicker * (0.9 + rag);
        const float edge_burn =
            saturate(reach - length(dc)) * 3.0;
        const float core = saturate(edge_burn - 0.6) * 2.5;
        result_rgb += float3(1.0, 0.45, 0.1) * edge_burn +
                      float3(1.0, 0.95, 0.85) * core * core;
    }
    const float4 result = float4(result_rgb, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
