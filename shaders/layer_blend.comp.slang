// Premultiplied composite: the layer's own alpha is the gate, so crop /
// scale-down / Shape coverage reveal the composite below even after the
// effect stack reshaped it. Blend modes act on unpremultiplied layer
// color, applied at the layer's coverage. Sequence lanes composite
// through the placement's canvas affine RIGHT HERE - Motion is an
// attribute of the composite, never an effect pass; use_xform 0 keeps
// the untransformed path identical for look layers.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint mode;         // doc::BlendMode
    float opacity;
    uint use_xform;
    float xf_scale;    // about canvas center, > 0
    float xf_rotate;   // radians
    float xf_shift_x;  // canvas fractions; +x right, +y down
    float xf_shift_y;
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
    float4 b;
    if (use_xform != 0u)
    {
        // Inverse of the monitor's forward map; rotation is rigid and
        // scale uniform, so the frame edge gets a one-pixel analytic AA
        // band like the layer transform's.
        const float aspect = float(width) / float(height);
        float2 p = (uv - 0.5 - float2(xf_shift_x, xf_shift_y))
                 * float2(aspect, 1.0);
        const float s = sin(-xf_rotate), c = cos(-xf_rotate);
        p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
        p /= max(xf_scale, 1e-4);
        const float2 suv = p / float2(aspect, 1.0) + 0.5;
        const float dx = min(suv.x, 1.0 - suv.x) * float(width) * xf_scale;
        const float dy = min(suv.y, 1.0 - suv.y) * float(height) * xf_scale;
        const float cov = saturate(min(dx, dy) + 0.5);
        b = layer.SampleLevel(clamp(suv, 0.0, 1.0), 0) * cov;
    }
    else
    {
        b = layer.SampleLevel(uv, 0);
    }
    b *= saturate(opacity);

    const float3 bu = b.rgb / max(b.a, 1.0e-6);
    float3 blended;
    switch (mode)
    {
    case 1: blended = a.rgb + bu; break;
    case 2: blended = a.rgb * bu; break;
    case 3: blended = fx_screen(a.rgb, bu); break;
    case 4: blended = abs(a.rgb - bu); break;
    default: blended = bu; break;
    }
    const float3 rgb = lerp(a.rgb, blended, b.a);
    const float alpha = a.a + b.a * (1.0 - a.a);
    output[tid.xy] = float4(rgb, alpha);
}
