// Encoded space: this is a codec artifact, not scene light.

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
    float radius;
    float rate;     // hz
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

    // Max gradient over a ring of probes, so the shimmer hugs BOTH sides
    // of an edge rather than only the side this pixel sits on.
    const float2 px = 1.0 / size;
    float edge = 0.0;
    [unroll]
    for (int k = 0; k < 4; ++k) {
        const float a = float(k) * 1.5708;
        const float2 off = float2(cos(a), sin(a)) * radius * px;
        const float2 p = clamp(uv + off, 0.0, 1.0);
        const float l0 = fx_luma(input.SampleLevel(
            clamp(p - float2(px.x, 0.0), 0.0, 1.0), 0).rgb);
        const float l1 = fx_luma(input.SampleLevel(
            clamp(p + float2(px.x, 0.0), 0.0, 1.0), 0).rgb);
        const float l2 = fx_luma(input.SampleLevel(
            clamp(p - float2(0.0, px.y), 0.0, 1.0), 0).rgb);
        const float l3 = fx_luma(input.SampleLevel(
            clamp(p + float2(0.0, px.y), 0.0, 1.0), 0).rgb);
        edge = max(edge, abs(l1 - l0) + abs(l3 - l2));
    }
    const float gate = saturate(edge * 5.0);

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = uint(t * max(rate, 0.01));
    // 2px cells, standing in for DCT basis residue.
    const float n = fx_hash01(fx_hash4(seed ^ tick * 0x9E37u, tick,
                                       tid.x / 2u, tid.y / 2u)) - 0.5;

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    enc = saturate(enc + n * gate * amount * 0.5);

    const float4 result = float4(fx_srgb_eotf(enc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
