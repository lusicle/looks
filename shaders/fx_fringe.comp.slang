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
    float amount;      // px at the frame edge, or edge-detect radius
    float mode;
    float threshold;
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

    float4 result;
    if (mode < 0.5) {
        const float2 d = uv - 0.5;
        const float s = amount / (0.5 * min(size.x, size.y));
        const float4 sr = input.SampleLevel(0.5 + d * (1.0 + s), 0);
        const float4 sb = input.SampleLevel(0.5 + d * (1.0 - s), 0);
        result = float4(sr.r, base.g, sb.b,
                        max(base.a, max(sr.a, sb.a)));
    } else {
        const float2 e = max(amount, 0.5) / size;
        const float l0 = fx_luma(saturate(base.rgb));
        const float lx =
            fx_luma(saturate(input.SampleLevel(uv + float2(e.x, 0), 0).rgb)) -
            fx_luma(saturate(input.SampleLevel(uv - float2(e.x, 0), 0).rgb));
        const float ly =
            fx_luma(saturate(input.SampleLevel(uv + float2(0, e.y), 0).rgb)) -
            fx_luma(saturate(input.SampleLevel(uv - float2(0, e.y), 0).rgb));
        const float edge = length(float2(lx, ly));
        const float gate = saturate((edge - threshold * 0.5) * 4.0);
        // The halo peaks just below clipping. This is deliberate.
        const float halo = saturate(1.0 - abs(l0 - 0.55) * 2.2);
        const float3 purple = float3(0.45, 0.1, 0.6);
        result = float4(base.rgb + purple * gate * halo,
                        max(base.a, gate * halo * 0.6));
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
