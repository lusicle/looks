// The transform gate is recomputed here as per-pixel coverage on
// opacity, so cropped or scaled-down layers reveal the composite below
// instead of stamping the transform pass's black over it.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint mode;         // doc::BlendMode
    float opacity;
    uint has_xform;    // 0 = identity, skip the gate
    float crop_l;
    float crop_r;
    float crop_t;
    float crop_b;
    uint flips;        // bit 0 = horizontal, bit 1 = vertical
    float scale;
    float rotate_rad;
};

[[vk::binding(0, 0)]] Sampler2D below;
[[vk::binding(1, 0)]] Sampler2D layer;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

// Keep in sync with layer_transform.comp.slang.
float xform_gate(float2 uv)
{
    const float aspect = float(width) / float(height);
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    const float s = sin(-rotate_rad), c = cos(-rotate_rad);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
    p /= max(scale, 1e-4);
    float2 suv = p / float2(aspect, 1.0) + 0.5;
    if ((flips & 1u) != 0u) suv.x = 1.0 - suv.x;
    if ((flips & 2u) != 0u) suv.y = 1.0 - suv.y;
    const float2 lo = float2(crop_l, crop_t);
    const float2 hi = float2(1.0 - crop_r, 1.0 - crop_b);
    const float dx = min(suv.x - lo.x, hi.x - suv.x) * float(width) * scale;
    const float dy = min(suv.y - lo.y, hi.y - suv.y) * float(height) * scale;
    return saturate(min(dx, dy) + 0.5);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float3 a = below.SampleLevel(uv, 0).rgb;
    const float3 b = layer.SampleLevel(uv, 0).rgb;

    float3 blended;
    switch (mode)
    {
    case 1: blended = a + b; break;
    case 2: blended = a * b; break;
    case 3: blended = 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b)); break;
    case 4: blended = abs(a - b); break;
    default: blended = b; break;
    }
    float op = saturate(opacity);
    if (has_xform != 0u)
        op *= xform_gate(uv);
    output[tid.xy] = float4(lerp(a, blended, op), 1.0);
}
