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
    float rows_n;
    float amount;
    float line_w;    // px
    float soften;
    float fill;
    float color;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Keep the band average. It dilutes 8-bit luma quantization steps.
float ridge_luma(Sampler2D tex, float u, float v, float blur_u,
                 float band_v)
{
    float4 center = tex.SampleLevel(float2(clamp(u, 0.0, 1.0), v), 0);
    if (band_v > 0.0) {
        const float4 upper = tex.SampleLevel(float2(
            clamp(u, 0.0, 1.0), clamp(v - band_v, 0.0, 1.0)), 0);
        const float4 lower = tex.SampleLevel(float2(
            clamp(u, 0.0, 1.0), clamp(v + band_v, 0.0, 1.0)), 0);
        center = center * 0.5 + (upper + lower) * 0.25;
    }
    if (blur_u <= 0.0)
        return fx_luma(fx_encoded(center));
    const float4 s1 = tex.SampleLevel(float2(clamp(u - blur_u, 0.0, 1.0), v), 0);
    const float4 s2 = tex.SampleLevel(float2(clamp(u + blur_u, 0.0, 1.0), v), 0);
    const float4 s3 = tex.SampleLevel(float2(clamp(u - 2.0 * blur_u, 0.0, 1.0), v), 0);
    const float4 s4 = tex.SampleLevel(float2(clamp(u + 2.0 * blur_u, 0.0, 1.0), v), 0);
    return fx_luma(fx_encoded(center * 0.4 + (s1 + s2) * 0.2 + (s3 + s4) * 0.1));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const uint n = uint(clamp(rows_n, 4.0, 120.0) + 0.5);
    const float spacing = size.y / (float(n) + 3.0);
    const float py = float(tid.y);
    const float lw = max(line_w, 0.5);
    const float blur_u = saturate(soften) * 10.0 / size.x;

    float3 col = base.rgb * (1.0 - saturate(fill));
    float sil = 1e9;

    // i = 0 is the bottom ridge, the nearest one.
    for (uint i = 0; i < n; ++i) {
        const float y_base = size.y - spacing * (1.5 + float(i));
        const float sv = 1.0 - (float(i) + 0.5) / float(n);
        const float band_v = 0.35 / float(n);
        const float l = ridge_luma(input, uv.x, sv, blur_u, band_v);
        const float height_px = saturate(amount) * spacing * 5.0;
        const float y_line = y_base - l * height_px;

        if (y_line < sil) {
            // Keep the slope term. Without it the line beads into dashes.
            const float du = 1.5 / size.x;
            const float slope_px =
                (fx_enc_luma(input, float2(clamp(uv.x + du, 0.0, 1.0), sv)) -
                 fx_enc_luma(input, float2(clamp(uv.x - du, 0.0, 1.0), sv))) *
                height_px / 3.0;
            const float dist =
                max(abs(py - y_line) - abs(slope_px) * 0.75, 0.0);
            if (dist < lw + 1.0) {
                const float a = saturate(1.0 - dist / lw);
                const float3 src = input.SampleLevel(
                    float2(uv.x, sv), 0).rgb;
                const float3 lc = lerp(float3(1.0, 1.0, 1.0), src * 1.5,
                                       saturate(color));
                col = max(col, lc * a * a);
            }
            sil = y_line;
        }
    }

    const float4 result =
        float4(col, max(base.a, saturate(fx_luma(col))));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
