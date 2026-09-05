// Band in encoded space so steps land perceptually even.

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
    float bands;
    float edge;
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

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float l = fx_luma(enc);

    const float n = clamp(round(bands), 2.0, 16.0);
    const float banded = (floor(l * n) + 0.5) / n;
    float3 col = l > 1.0e-4 ? enc * (banded / l) : float3(banded, banded, banded);

    const float2 e = 1.5 / size;
    const float gx = fx_enc_luma(input, uv + float2(e.x, 0)) - fx_enc_luma(input, uv - float2(e.x, 0));
    const float gy = fx_enc_luma(input, uv + float2(0, e.y)) - fx_enc_luma(input, uv - float2(0, e.y));
    const float line = smoothstep(0.12, 0.3, length(float2(gx, gy)));
    col *= 1.0 - line * edge;

    const float4 result = float4(fx_srgb_eotf(saturate(col)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
