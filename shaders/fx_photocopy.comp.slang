// Photocopy: copy-of-a-copy tone collapse. Each generation squeezes the
// tones harder around the drum threshold; roller streaks wobble that
// threshold per column, toner speckles the whites and drops out of the
// blacks. Display-referred (a print look), self-converting.
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
    float contrast;      // collapse hardness per generation
    float generations;   // stacked copies
    float toner;         // speckle + dropout amount
    float streaks;       // per-column threshold wobble
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

    float g = fx_luma(fx_srgb_oetf(saturate(base.rgb)));

    // Roller streaks: slow per-column threshold wobble, static in time.
    const float streak_n =
        fx_value_noise1(seed ^ 0x51EDu, float(tid.x) * 0.02) +
        0.35 * fx_value_noise1(seed ^ 0xA3u, float(tid.x) * 0.11);
    const float thresh =
        0.55 + (streak_n / 1.35 - 0.5) * 0.4 * saturate(streaks);

    // Generation loop: smoothstep window narrows every copy.
    const uint gens = uint(clamp(generations, 1.0, 8.0) + 0.5);
    for (uint i = 0; i < gens; ++i) {
        const float w =
            lerp(0.5, 0.05, saturate(contrast)) / (1.0 + float(i) * 0.7);
        g = smoothstep(thresh - w, thresh + w, g);
    }

    // Toner: each video frame is a fresh copy, but re-seed gently (8 hz).
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = uint(t * 8.0);
    const uint h = fx_hash4(seed ^ (tick + 1u) * 0x9E37u, tid.x, tid.y, tick);
    const float sp = fx_hash01(h);
    if (sp < toner * 0.05 * g)
        g = 0.08;                       // dark speck on the whites
    else if (sp > 1.0 - toner * 0.06 * (1.0 - g))
        g = lerp(g, 0.75, 0.8);         // toner failed to stick

    // Warm paper white, slightly cool toner black.
    const float3 enc = lerp(float3(0.05, 0.05, 0.07),
                            float3(0.97, 0.96, 0.93), g);

    const float4 result = float4(fx_srgb_eotf(saturate(enc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
