// Gather, not scatter: each output pixel scans the candidate source
// lines whose luma-displaced trace could land on it.

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
    float spacing;   // px
    float amount;    // max displacement, px
    float line_w;    // half-thickness, px
    float mode;      // 0 rows displaced up, 1 cols displaced left
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
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const bool cols = mode >= 0.5;
    const float sp = max(spacing, 2.0);
    // p is perpendicular to the lines (the displacement axis), along runs
    // down them (the axis wiggle and beam ride).
    const float p = cols ? float(tid.x) + 0.5 : float(tid.y) + 0.5;
    const float along = cols ? float(tid.y) + 0.5 : float(tid.x) + 0.5;
    const float p_max = cols ? size.x : size.y;
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float wig_max = wiggle * amount * 0.4;
    // Wrapped into one spacing so the crawling grid stays seamless.
    const float phase = frac(t * scroll / sp) * sp;

    // Bounds of the lines whose displaced trace can reach p.
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
        const float l = fx_luma(fx_srgb_oetf(saturate(src)));
        const float wig = wiggle > 0.001
            ? (fx_value_noise1(seed ^ uint(s + 4096) * 197u,
                               along * 0.035 + t * 3.0) - 0.5) *
                  2.0 * wig_max
            : 0.0;
        const float displaced = line_p - l * amount + wig;
        const float d = abs(p - displaced);
        if (d < line_w) {
            // Weak signal shortens the dash duty until the trace beads.
            if (beam > 0.001) {
                const float period = 3.0 + line_w * 2.0;
                const float phase_off =
                    fx_hash01(fx_hash3(seed, uint(s + 4096), 3u)) * period;
                const float duty =
                    lerp(1.0, 0.12 + 0.88 * l * l, saturate(beam));
                if (frac((along + phase_off) / period) > duty) continue;
            }
            const float glow = 1.0 - d / line_w;
            col = max(col, src * (0.35 + 0.65 * l) * (glow * glow) * 2.0);
            // The drawn trace is the content: its coverage is the line
            // glow gated by the source pixel's own coverage.
            col_a = max(col_a,
                        saturate((glow * glow) * 2.0) * src4.a);
        }
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
