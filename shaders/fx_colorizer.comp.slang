// Encoded-domain mapping, and unlike the Quantizer's palettes this ramp
// is continuous.

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
    float sh_hue;    // stop at black
    float mid_hue;   // stop at middle gray
    float hi_hue;    // stop at white
    float sat;
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

    const float l = fx_luma(fx_srgb_oetf(saturate(base.rgb)));

    // The stops keep a rising luminance ramp so tonality survives, and
    // the dark stop stays near-black so the hue reads in the low mids
    // rather than lifting the floor into a wash.
    const float3 c0 = fx_hue_to_rgb(sh_hue) * 0.12;
    const float3 c1 = fx_hue_to_rgb(mid_hue) * 0.6;
    const float3 c2 =
        lerp(fx_hue_to_rgb(hi_hue), float3(1.0, 1.0, 1.0), 0.35);
    float3 ramp = l < 0.5 ? lerp(c0, c1, l * 2.0)
                          : lerp(c1, c2, (l - 0.5) * 2.0);
    ramp = lerp(float3(l, l, l), ramp, saturate(sat));

    const float4 result = float4(fx_srgb_eotf(saturate(ramp)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
