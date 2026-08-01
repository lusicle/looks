// Text (docs/flow_canvas.md): runtime-TTF text burn-in. The
// engine rasterizes the whole string into a single-channel signed
// distance bitmap (in-repo TrueType loader, one raster per size BUCKET
// so keyframed size scales between cached rasters), and this kernel
// samples it with the standard SDF decode — analytic screen-px AA band,
// distance-based outline, crisp at any size/rotation. Proxy-safe: size
// reads as px at 1080 frame height.
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float wet;
    float opacity;
    uint seed;
    uint frame;
    float fps;
    float font;        // engine-side font pick; unread here
    float size;        // px at 1080p frame height
    float pos_x;
    float pos_y;
    float angle;       // degrees
    float hue;
    float sat;
    float lite;
    float outline;
    float bm_w;        // extra words (engine): SDF bitmap px dims
    float bm_h;
    float spread_px;   // SDF encode range, raster px
    float raster_px;   // the bucket's font size (raster scale)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D sdf;   // R8, 0.5 = edge, inside > 0.5
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 dim = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / dim;
    const float4 base = input.SampleLevel(uv, 0);
    output[tid.xy] = base;
    if (bm_w < 1.0)
        return;

    // Screen px per raster px; size reads at 1080-reference height so
    // the proxy divisor shrinks everything cleanly.
    const float size_px = max(size, 1.0) * (float(height) / 1080.0);
    const float rscale = size_px / max(raster_px, 1.0);

    // Pixel -> raster space: undo position, rotation, scale; the string
    // box (bitmap) is centered on the anchor.
    float2 p = float2(tid.xy) + 0.5 - float2(pos_x, pos_y) * dim;
    const float a = -angle * 0.01745329252;
    const float ca = cos(a), sa = sin(a);
    p = float2(p.x * ca - p.y * sa, p.x * sa + p.y * ca);
    p /= rscale;

    const float2 half_bm = float2(bm_w, bm_h) * 0.5;
    if (abs(p.x) >= half_bm.x || abs(p.y) >= half_bm.y)
        return;
    const float2 suv = (p + half_bm) / float2(bm_w, bm_h);

    // SDF decode: raster-px distance scaled to screen px.
    const float s = sdf.SampleLevel(suv, 0).r;
    const float sd_screen = (s - 0.5) * 2.0 * spread_px * rscale;
    const float outline_px = outline * size_px * 0.08;
    const float cov = saturate(sd_screen + 0.5);
    const float cov_out = saturate(sd_screen + 0.5 + outline_px);
    if (cov_out <= 0.0)
        return;

    // Display-referred compose: dark rim under the fill color.
    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float3 fill = lerp(float3(1.0, 1.0, 1.0),
                             fx_hue_to_rgb(hue), saturate(sat)) *
                        saturate(lite);
    float3 c = lerp(enc, float3(0.02, 0.02, 0.02), cov_out);
    c = lerp(c, fill, cov);

    const float4 result = float4(fx_srgb_eotf(saturate(c)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
