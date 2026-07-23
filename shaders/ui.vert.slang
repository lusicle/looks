// UI vertex shader. Positions arrive in PHYSICAL pixels; the push-constant
// projection maps framebuffer top-left to NDC (-1,-1). Vertex color bytes
// are sRGB-encoded RGB + linear alpha (Color::to_rgba8) — decoded here once
// so interpolation and blending stay linear. Keep in sync with text.vert.

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
    // shape = { radius, stroke, halfW, halfH } in physical px; all zero on
    // pass-through primitives.
    [[vk::location(3)]] float4 a_shape : TEXCOORD1;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 v_uv    : TEXCOORD0;
    [[vk::location(1)]] float4 v_color : COLOR0;
    [[vk::location(2)]] float4 v_shape : TEXCOORD1;
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
    o.v_shape     = input.a_shape;
    return o;
}
