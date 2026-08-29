[[vk::push_constant]]
cbuffer Push
{
    float2 scale;    // fitted quad size in NDC
    float2 offset;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput main(uint vertex_id : SV_VertexID)
{
    // One oversized triangle, not a quad. uv lands in 0 to 1.
    const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    VSOutput o;
    o.v_uv = uv;
    const float2 ndc = (uv * 2.0 - 1.0) * scale + offset;
    o.sv_position = float4(ndc, 0.0, 1.0);
    return o;
}
