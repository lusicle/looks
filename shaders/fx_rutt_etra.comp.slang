// Gather, do not scatter. Each pixel scans the candidate lines.

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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float spacing;   // px
    float amount;    // max displacement, px
    float line_w;    // half-thickness, px
    float mode;
    float wiggle;
    float beam;
    float scroll;    // px/s
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(canvas_w, canvas_h);
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float2 material_px = uv * size;
    const float footprint = canvas_w / float(width);
    const float4 base = input.SampleLevel(uv, 0);

    const bool cols = mode >= 0.5;
    const float sp = max(spacing, 2.0);
    // p is across the lines. along runs down them.
    const float p = cols ? material_px.x : material_px.y;
    const float along = cols ? material_px.y : material_px.x;
    const float p_max = cols ? size.x : size.y;
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float wig_max = wiggle * amount * 0.4;
    const float phase = frac(t * scroll / sp) * sp;

    const int first = int(ceil((p - wig_max - phase) / sp)) - 1;
    const int last = int(floor((p + amount + wig_max - phase) / sp)) + 1;
    float3 col = 0.0;
    float col_a = 0.0;
    for (int s = first; s <= last; ++s) {
        const float line_p = float(s) * sp + sp * 0.5 + phase;
        if (line_p < 0.5) continue;
        if (line_p >= p_max) break;
        const float2 suv = cols
            ? float2((line_p + 0.5) / size.x, uv.y)
            : float2(uv.x, (line_p + 0.5) / size.y);
        const float4 src4 = input.SampleLevel(suv, 0);
        const float3 src = src4.rgb;
        const float l = fx_luma(fx_encoded(src4));
        const float wig = wiggle > 0.001
            ? (fx_value_noise1(seed ^ uint(s + 4096) * 197u,
                               along * 0.035 + t * 3.0) - 0.5) *
                  2.0 * wig_max
            : 0.0;
        const float displaced = line_p - l * amount + wig;
        const float2 step_uv = cols ? float2(0.0, footprint / size.y) : float2(footprint / size.x, 0.0);
        const float next_luma = fx_enc_luma(input, suv + step_uv);
        const float slope = (next_luma - l) * amount / footprint;
        const float d = abs(p - displaced) / sqrt(1.0 + slope * slope);
        if (d < line_w + footprint) {
            if (beam > 0.001) {
                const float period = 3.0 + line_w * 2.0;
                const float phase_off =
                    fx_hash01(fx_hash3(seed, uint(s + 4096), 3u)) * period;
                const float duty =
                    lerp(1.0, 0.12 + 0.88 * l * l, saturate(beam));
                if (frac((along + phase_off) / period) > duty) continue;
            }
            const float glow = (1.0 - smoothstep(max(0.0, line_w - footprint * 0.5), line_w + footprint * 0.5, d)) *
                min(1.0, line_w / footprint);
            col += src * (0.35 + 0.65 * l) * (glow * glow) * 2.0;
            // The trace coverage is the glow gated by the source coverage.
            col_a = max(col_a,
                        saturate((glow * glow) * 2.0) * src4.a);
        }
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
