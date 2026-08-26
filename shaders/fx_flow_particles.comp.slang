// History is gathered by sampling BACKWARD along the flow, so no scatter
// or atomics are needed to advect the field.

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
    float advect;
    float dissolve;   // fraction of pixels re-injected per frame
    float decay;
    float swirl;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;
[[vk::binding(2, 0)]] Sampler2D flow;    // block MVs in px
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
    // Varies smoothly across the frame so trails curl rather than
    // translating as a block.
    const float bend =
        swirl * (fx_value_noise1(seed, uv.x * 7.0 + uv.y * 11.0 +
                                           float(frame) * 0.01) - 0.5) * 2.0;
    mv += float2(-mv.y, mv.x) * bend;
    // Idle drift, so still footage still flows a little.
    mv += float2(0.4, -0.25);

    const float2 suv = clamp(uv - mv * advect / size, 0.0, 1.0);
    const float4 hist4 = prev.SampleLevel(suv, 0);
    const float3 history = hist4.rgb * decay;
    const float hist_a = hist4.a * decay;

    const float gate =
        fx_hash01(fx_hash4(seed, frame, tid.x, tid.y));
    const float3 fx_rgb = gate < dissolve ? base.rgb : history;
    const float fx_a = gate < dissolve ? base.a : hist_a;

    const float4 result = float4(fx_rgb, fx_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
