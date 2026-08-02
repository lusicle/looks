// Display-referred core.

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
    float levels;
    float mode;   // 0 rgb, 1 luma-only banding
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
    const float n = max(levels, 2.0);
    float3 c;
    if (mode >= 0.5) {
        // Adding the luma delta carries the chroma vector across intact.
        const float l = fx_luma(enc);
        const float ql = round(l * (n - 1.0)) / (n - 1.0);
        c = saturate(enc + (ql - l));
    } else {
        c = round(enc * (n - 1.0)) / (n - 1.0);
    }

    const float4 result = float4(fx_srgb_eotf(saturate(c)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
