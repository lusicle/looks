// Flow-field particles: the frame dissolves into a persistent
// particle field advected along the optical flow. A seeded per-pixel gate
// injects fresh pixels each frame; everything else is history pulled back
// along the motion (with optional swirl) and decayed — moving subjects
// shed streaming trails of themselves.
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
    float advect;     // flow multiplier
    float dissolve;   // fraction of pixels re-injected per frame
    float decay;      // history energy retention
    float swirl;      // perpendicular curl of the advection
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;    // own previous output
[[vk::binding(2, 0)]] Sampler2D flow;    // block MVs in px, (w/16, h/16)
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    float2 mv = flow.SampleLevel(uv, 0).rg;
    // Swirl: bend the advection sideways, varying smoothly across the
    // frame so trails curl instead of translating.
    const float bend =
        swirl * (fx_value_noise1(seed, uv.x * 7.0 + uv.y * 11.0 +
                                           float(frame) * 0.01) - 0.5) * 2.0;
    mv += float2(-mv.y, mv.x) * bend;
    // Idle drift so still footage still flows a little.
    mv += float2(0.4, -0.25);

    const float2 suv = clamp(uv - mv * advect / size, 0.0, 1.0);
    const float3 history = prev.SampleLevel(suv, 0).rgb * decay;

    const float gate =
        fx_hash01(fx_hash4(seed, frame, tid.x, tid.y));
    const float3 fx_rgb = gate < dissolve ? base.rgb : history;

    const float4 result = float4(fx_rgb, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
