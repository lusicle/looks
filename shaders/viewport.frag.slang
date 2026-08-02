// No OETF here: the sRGB swapchain encodes on write. Along with export,
// this is one of only two places the encode happens.

[[vk::binding(0, 0)]] Sampler2D frame;

struct FSInput
{
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(FSInput input) : SV_Target
{
    return float4(frame.Sample(input.v_uv).rgb, 1.0);
}
