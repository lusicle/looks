// Positions arrive in PHYSICAL pixels. Vertex color bytes are sRGB RGB
// with linear alpha, decoded here once so interpolation and blending stay
// linear.

#include "fx_common.slang"

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
    [[vk::location(3)]] float4 a_shape : TEXCOORD1;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 v_uv    : TEXCOORD0;
    [[vk::location(1)]] float4 v_color : COLOR0;
    [[vk::location(2)]] float4 v_shape : TEXCOORD1;
};

[shader("vertex")]
VSOutput main(VSInput input)
{
    VSOutput o;
    o.sv_position = mul(projection, float4(input.a_pos, 0.0, 1.0));
    o.v_uv        = input.a_uv;
    o.v_color     = float4(fx_srgb_eotf(input.a_color.rgb), input.a_color.a);
    o.v_shape     = input.a_shape;
    return o;
}
