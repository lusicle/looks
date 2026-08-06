// Premultiplied composite: the layer's own alpha is the gate, so crop /
// scale-down / Shape coverage reveal the composite below even after the
// effect stack reshaped it. Blend modes act on unpremultiplied layer
// color, applied at the layer's coverage.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint mode;         // doc::BlendMode
    float opacity;
};

[[vk::binding(0, 0)]] Sampler2D below;
[[vk::binding(1, 0)]] Sampler2D layer;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 a = below.SampleLevel(uv, 0);
    float4 b = layer.SampleLevel(uv, 0);
    b *= saturate(opacity);

    const float3 bu = b.rgb / max(b.a, 1.0e-6);
    float3 blended;
    switch (mode)
    {
    case 1: blended = a.rgb + bu; break;
    case 2: blended = a.rgb * bu; break;
    case 3:
        blended = 1.0 - (1.0 - saturate(a.rgb)) * (1.0 - saturate(bu));
        break;
    case 4: blended = abs(a.rgb - bu); break;
    default: blended = bu; break;
    }
    const float3 rgb = lerp(a.rgb, blended, b.a);
    const float alpha = a.a + b.a * (1.0 - a.a);
    output[tid.xy] = float4(rgb, alpha);
}
