// Viewport blit fragment: sample the linear RGBA16F result; the sRGB
// swapchain performs the OETF encode on write (spec §4: OETF only at the
// viewport blit and export).

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
