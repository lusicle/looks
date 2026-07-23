// Text vertex shader — identical transform/color model to ui.vert; UV
// addresses the glyph atlas directly.

[[vk::push_constant]]
cbuffer Push
{
    float4x4 projection;
};

struct VSInput
{
    [[vk::location(0)]] float2 a_pos   : POSITION;
    [[vk::location(1)]] float2 a_uv    : TEXCOORD0;
    [[vk::location(2)]] float4 a_color : COLOR0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 v_uv    : TEXCOORD0;
    [[vk::location(1)]] float4 v_color : COLOR0;
};

float3 srgb_to_linear(float3 c)
{
    const float3 hi = pow((c + 0.055) / 1.055, 2.4);
    const float3 lo = c / 12.92;
    return lerp(hi, lo, step(c, 0.04045));
}

[shader("vertex")]
VSOutput main(VSInput input)
{
    VSOutput o;
    o.sv_position = mul(projection, float4(input.a_pos, 0.0, 1.0));
    o.v_uv        = input.a_uv;
    o.v_color     = float4(srgb_to_linear(input.a_color.rgb), input.a_color.a);
    return o;
}
