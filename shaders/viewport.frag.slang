// No OETF here: the sRGB swapchain encodes on write. Along with export,
// this is one of only two places the encode happens. Composite alpha is
// flattened over black (premultiplied: just rgb) or over a checker.

[[vk::push_constant]]
cbuffer Push
{
    // Vertex words first; this stage's range covers only alpha_mode.
    float2 vp_scale;
    float2 vp_offset;
    uint alpha_mode;   // 0 flatten over black, 1 checkerboard
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
        // 8 px screen-space checker, dark gray pair in linear light.
        const uint2 cell = uint2(input.pos.xy) >> 3;
        const float g = ((cell.x + cell.y) & 1u) != 0u ? 0.055 : 0.11;
        return float4(c.rgb + g * (1.0 - saturate(c.a)), 1.0);
    }
    return float4(c.rgb, 1.0);
}
