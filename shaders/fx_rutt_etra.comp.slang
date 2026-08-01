// Rutt-Etra: the frame becomes raster scanlines displaced by
// luma — bright areas push their line into a relief. Mode picks row or
// column rasters; `wiggle` rides oscillator noise on every trace (the
// unstable-deflection look), and `beam` breaks the trace into beads where
// the signal is weak (beam-intensity modulation). For each output pixel,
// scan the candidate source lines that could land here and light up where
// a displaced trace passes. Black elsewhere, trace color from source.
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
    float spacing;   // px between scanlines
    float amount;    // max displacement, px
    float line_w;    // line half-thickness, px
    float mode;      // 0 = rows displaced up, 1 = cols displaced left
    float wiggle;    // oscillator noise riding the trace
    float beam;      // dot breakup in low-signal regions
    float scroll;    // raster crawl, px/s (frame-clocked)
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
    // Perpendicular position (which the trace displaces along) and the
    // along-line coordinate (which the wiggle and beam pattern ride).
    const float p = cols ? float(tid.x) + 0.5 : float(tid.y) + 0.5;
    const float along = cols ? float(tid.y) + 0.5 : float(tid.x) + 0.5;
    const float p_max = cols ? size.x : size.y;
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float wig_max = wiggle * amount * 0.4;
    // Raster crawl: the whole line grid drifts on the timeline clock —
    // the animated scan-render. Wrapped into one spacing so the grid is
    // seamless; deterministic (frame-clocked, ).
    const float phase = frac(t * scroll / sp) * sp;

    // Lines whose displaced trace can reach p.
    const int first = int(ceil((p - wig_max - phase) / sp)) - 1;
    const int last = int(floor((p + amount + wig_max - phase) / sp)) + 1;
    float3 col = 0.0;
    for (int s = first; s <= last; ++s) {
        const float line_p = float(s) * sp + sp * 0.5 + phase;
        if (line_p < 0.5) continue;
        if (line_p >= p_max) break;
        const float2 suv = cols
            ? float2((line_p + 0.5) / size.x, uv.y)
            : float2(uv.x, (line_p + 0.5) / size.y);
        const float3 src = input.SampleLevel(suv, 0).rgb;
        const float l = fx_luma(fx_srgb_oetf(saturate(src)));
        // Oscillator noise on the deflection: per-line phase, drifting on
        // the timeline clock — the trace never sits still.
        const float wig = wiggle > 0.001
            ? (fx_value_noise1(seed ^ uint(s + 4096) * 197u,
                               along * 0.035 + t * 3.0) - 0.5) *
                  2.0 * wig_max
            : 0.0;
        const float displaced = line_p - l * amount + wig;
        const float d = abs(p - displaced);
        if (d < line_w) {
            // Beam-intensity dot breakup: weak signal shortens the dash
            // duty until the trace beads apart.
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
        }
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
