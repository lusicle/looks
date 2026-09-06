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
    float pitch;
    float horizon;   // screen position, 0 = top
    float line_w;    // px
    float grid;
    float color;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Keep the band average. It dilutes 8-bit luma quantization steps.
float wt_luma(Sampler2D tex, float2 suv, float band_v)
{
    const float4 center = tex.SampleLevel(suv, 0);
    const float4 upper = tex.SampleLevel(
        float2(suv.x, clamp(suv.y - band_v, 0.0, 1.0)), 0);
    const float4 lower = tex.SampleLevel(
        float2(suv.x, clamp(suv.y + band_v, 0.0, 1.0)), 0);
    return fx_luma(fx_encoded(center * 0.5 + (upper + lower) * 0.25));
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

    const uint n = uint(clamp(rows_n, 4.0, 200.0) + 0.5);
    const float horizon_y = saturate(horizon) * size.y;
    const float py = float(tid.y);
    const float lw = max(line_w, 0.5);

    float3 col = float3(0.0, 0.0, 0.0);
    float sil = 1e9;   // nearest silhouette so far, in screen y

    for (uint i = 0; i < n; ++i) {
        const float d = (float(i) + 0.5) / float(n);   // 0 near, 1 far
        const float persp = pow(d, lerp(1.0, 2.2, saturate(pitch)));
        const float su =
            0.5 + (uv.x - 0.5) * lerp(1.0, 2.0, d * saturate(pitch));
        if (su < 0.0 || su > 1.0) continue;
        const float sv = 1.0 - d;

        const float band_v = 0.35 / float(n);
        const float l = wt_luma(input, float2(su, sv), band_v);
        const float3 src =
            input.SampleLevel(float2(su, sv), 0).rgb;

        const float y_base = lerp(size.y - 1.0, horizon_y, persp);
        const float height_px =
            saturate(amount) * lerp(0.28, 0.05, persp) * size.y;
        const float y_line = y_base - l * height_px;

        if (y_line < sil) {
            // Keep the slope term. Without it the wire beads into dashes.
            const float dsu =
                (1.5 / size.x) * lerp(1.0, 2.0, d * saturate(pitch));
            const float slope_px =
                (wt_luma(input, float2(clamp(su + dsu, 0.0, 1.0), sv),
                         band_v) -
                 wt_luma(input, float2(clamp(su - dsu, 0.0, 1.0), sv),
                         band_v)) *
                height_px / 3.0;
            const float dist =
                max(abs(py - y_line) - abs(slope_px) * 0.75, 0.0);
            if (dist < lw + 1.0) {
                const float a = saturate(1.0 - dist / lw);
                float3 lc = lerp(float3(0.25, 1.0, 0.45), src * 1.5,
                                 saturate(color));
                const float gu = frac(su * 24.0);
                const float tick_v = min(gu, 1.0 - gu) < 0.045 ? 1.0 : 0.0;
                lc *= 1.0 + grid * tick_v * 0.9;
                const float fade = lerp(1.0, 0.3, persp);
                col = max(col, lc * a * a * fade);
            }
            sil = y_line;
        }
    }

    const float4 result =
        float4(col, max(base.a, saturate(fx_luma(col))));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
