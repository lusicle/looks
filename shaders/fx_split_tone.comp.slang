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
    float sh_hue;
    float sh_amt;
    float hi_hue;
    float hi_amt;
    float balance;   // -1 favors shadows, +1 favors highlights
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

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float l = fx_luma(enc);
    const float pivot = saturate(0.5 + balance * 0.3);
    const float w_hi = smoothstep(pivot - 0.35, pivot + 0.35, l);
    const float w_sh = 1.0 - w_hi;

    const float3 sh_vec = fx_hue_to_rgb(sh_hue) - float3(0.5, 0.5, 0.5);
    const float3 hi_vec = fx_hue_to_rgb(hi_hue) - float3(0.5, 0.5, 0.5);
    enc += sh_vec * w_sh * sh_amt * 0.35;
    enc += hi_vec * w_hi * hi_amt * 0.35;

    // Re-anchor the luma so the tone shift stays chroma only.
    const float l2 = fx_luma(saturate(enc));
    enc = saturate(enc + (l - l2));

    const float4 result = float4(fx_srgb_eotf(enc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
