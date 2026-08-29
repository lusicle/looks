// Do not apply an OETF here. The sRGB swapchain encodes on write.

[[vk::push_constant]]
cbuffer Push
{
    // Vertex words come first. This stage covers only alpha_mode.
    float2 vp_scale;
    float2 vp_offset;
    uint alpha_mode;
};

[[vk::binding(0, 0)]] Sampler2D frame;

struct FSInput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(FSInput input) : SV_Target
{
    const float4 c = frame.Sample(input.v_uv);
    if (alpha_mode == 1u)
    {
        // 8 px screen-space checker in linear light.
        const uint2 cell = uint2(input.pos.xy) >> 3;
        const float g = ((cell.x + cell.y) & 1u) != 0u ? 0.055 : 0.11;
        return float4(c.rgb + g * (1.0 - saturate(c.a)), 1.0);
    }
    return float4(c.rgb, 1.0);
}
