// Bit-plane glitch: quantize to 8-bit and run a bitwise op (XOR / AND /
// OR) with a fixed pattern value on selected channels — posterized bands,
// inverted plateaus, and hard contour rings depending on which bits the
// pattern touches. Display-referred by definition (bits live on the
// encoded signal).
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
    float op;         // 0 xor, 1 and, 2 or
    float value;      // pattern byte 1..255
    float channels;   // 0 rgb, 1 r, 2 g, 3 b
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    uint3 q = uint3(enc * 255.0 + 0.5);
    const uint v = uint(clamp(value, 1.0, 255.0));
    const uint o = uint(op + 0.5);
    const uint ch = uint(channels + 0.5);

    uint3 g = q;
    if (o == 0u) g = q ^ uint3(v, v, v);
    else if (o == 1u) g = q & uint3(v, v, v);
    else g = q | uint3(v, v, v);

    if (ch == 1u) q.x = g.x;
    else if (ch == 2u) q.y = g.y;
    else if (ch == 3u) q.z = g.z;
    else q = g;

    const float3 outc = float3(q) / 255.0;
    const float4 result = float4(fx_srgb_eotf(outc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
