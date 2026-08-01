// Analog snow + ghosting: sparse white/dark RF speckle bursts
// re-seeded every frame, plus a displaced low-contrast ghost image
// (multipath reflection). Encoded space — snow rides on the signal.
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
    float amount;     // snow density/intensity
    float ghost_px;   // ghost displacement
    float ghost;      // ghost strength
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

    float3 enc = fx_srgb_oetf(saturate(base.rgb));

    // Ghost: a shifted, washed-out copy added on top.
    const float2 guv = clamp(uv + float2(ghost_px, 0.0) / size, 0.0, 1.0);
    const float3 ghost_enc =
        fx_srgb_oetf(saturate(input.SampleLevel(guv, 0).rgb));
    enc = lerp(enc, max(enc, ghost_enc), saturate(ghost));

    // Snow: heavy-tailed hash — most pixels untouched, a few flare hard.
    // Streak slightly horizontal (2px cells) like RF noise.
    const uint h = fx_hash4(seed, frame, tid.x >> 1, tid.y);
    const float u = fx_hash01(h);
    const float spark = pow(u, 24.0);   // rare bright events
    const float dark = pow(fx_hash01(fx_hash(h ^ 0x9E3779B9u)), 40.0);
    enc = saturate(enc + spark * amount * 1.6 - dark * amount * 0.8);

    const float4 result = float4(fx_srgb_eotf(enc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
