// The composite is premultiplied. The layer alpha is the gate.
// Blend modes act on unpremultiplied color at the layer coverage.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint mode;
    float opacity;
    uint use_xform;
    float xf_scale;    // about the anchor, > 0
    float xf_rotate;   // radians
    float xf_shift_x;  // canvas fractions, +x right and +y down
    float xf_shift_y;
    float anchor_x;    // scale and rotate pivot, uv
    float anchor_y;
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
        const float aspect = float(width) / float(height);
        const float2 anchor = float2(anchor_x, anchor_y);
        float2 p = (uv - anchor - float2(xf_shift_x, xf_shift_y))
                 * float2(aspect, 1.0);
        const float s = sin(-xf_rotate), c = cos(-xf_rotate);
        p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
        p /= max(xf_scale, 1e-4);
        const float2 suv = p / float2(aspect, 1.0) + anchor;
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

    output[tid.xy] = fx_blend(a, b, mode);
}
