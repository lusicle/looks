// Layer transform: crop / flip / scale / rotate, applied to the
// layer source before its effect stack. Inverse-maps each output pixel
// into the source: rotate and scale about the frame center (aspect-
// corrected so rotation stays rigid), then flips, then the crop gate.
// Outside the transformed region the layer is opaque black — the blend
// pass recomputes the same gate analytically to reveal the composite
// below (this compositor carries no alpha through effect chains).

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float crop_l;
    float crop_r;
    float crop_t;
    float crop_b;
    uint flips;        // bit 0 = horizontal, bit 1 = vertical
    float scale;       // about frame center, > 0
    float rotate_rad;
};

[[vk::binding(0, 0)]] Sampler2D source;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Shared with layer_blend.comp.slang (keep in sync): returns the source
// uv for an output uv plus the crop/frame coverage in [0, 1] with a
// one-pixel anti-aliased edge.
float2 xform_source_uv(float2 uv, float aspect)
{
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    const float s = sin(-rotate_rad), c = cos(-rotate_rad);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
    p /= max(scale, 1e-4);
    float2 suv = p / float2(aspect, 1.0) + 0.5;
    if ((flips & 1u) != 0u) suv.x = 1.0 - suv.x;
    if ((flips & 2u) != 0u) suv.y = 1.0 - suv.y;
    return suv;
}

float xform_coverage(float2 suv)
{
    // Distance to the crop window in source uv; rotation is rigid and
    // scale is uniform, so one output pixel spans (1 / (dim * scale)) uv.
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
    const float aspect = float(width) / float(height);
    const float2 suv = xform_source_uv(uv, aspect);
    const float cov = xform_coverage(suv);
    const float3 rgb = source.SampleLevel(clamp(suv, 0.0, 1.0), 0).rgb;
    output[tid.xy] = float4(rgb * cov, 1.0);
}
