// Display-referred.

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
    float r_from;   // 0 r, 1 g, 2 b, 3 luma
    float g_from;
    float b_from;
    float amount;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float pick_channel(float4 src, float sel)
{
    const int s = int(sel + 0.5);
    if (s == 1) return src.g;
    if (s == 2) return src.b;
    if (s == 3) return src.a;   // luma is packed into .a by the caller
    return src.r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float4 src = float4(enc, fx_luma(enc));
    const float3 mixed = float3(pick_channel(src, r_from),
                                pick_channel(src, g_from),
                                pick_channel(src, b_from));
    const float3 c = lerp(enc, mixed, saturate(amount));

    const float4 result = float4(fx_srgb_eotf(saturate(c)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
