// The beam and the halation sum in linear light.

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
    float curvature;
    float scanline;
    float lines;       // virtual scanline count
    float bloom;
    float mask_mode;
    float mask_amt;
    float triad;       // px
    float converge;    // px
    float halation;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// sx holds the r, g and b landing positions as x in uv space.
// dist is the distance to the lit point, in line pitches.
void beam_line(float3 sx, float line_y, float dist, float bloom_amt,
               out float3 lvl, out float3 w, out float wgt_a)
{
    const float4 sg = input.SampleLevel(float2(sx.y, line_y), 0);
    const float r = input.SampleLevel(float2(sx.x, line_y), 0).r;
    const float b = input.SampleLevel(float2(sx.z, line_y), 0).b;
    lvl = float3(r, sg.g, b);
    const float3 sig = 0.30 + bloom_amt * 0.35 *
                       pow(saturate(lvl), float3(0.6, 0.6, 0.6));
    w = exp(-0.5 * (dist * dist) / (sig * sig));
    wgt_a = sg.a * max(w.r, max(w.g, w.b));
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

    const int mm = int(mask_mode + 0.5);
    const bool lcd = mm == 3;

    float2 c = uv * 2.0 - 1.0;
    const float k = lcd ? 0.0 : curvature * 0.22;
    c *= 1.0 + k * dot(c, c);
    const float2 suv = c * 0.5 + 0.5;
    const bool off_screen = any(suv < 0.0) || any(suv > 1.0);

    float3 col = float3(0.0, 0.0, 0.0);
    float col_a = 0.0;
    if (!off_screen) {
        if (lcd) {
            const float4 s = input.SampleLevel(suv, 0);
            col = s.rgb;
            col_a = s.a;
            const float tw = max(triad, 2.0);
            const float cx = frac(float(tid.x) / (tw * 3.0)) * 3.0;
            const uint stripe = uint(cx) % 3u;
            const float in_stripe = frac(cx);
            const float border =
                smoothstep(0.0, 0.15, in_stripe) *
                (1.0 - smoothstep(0.85, 1.0, in_stripe));
            float3 sub = float3(0.08, 0.08, 0.08);
            if (stripe == 0u) sub.r = 1.0;
            else if (stripe == 1u) sub.g = 1.0;
            else sub.b = 1.0;
            const float ry = frac(float(tid.y) / tw);
            const float row_gap =
                1.0 - saturate(scanline) * smoothstep(0.82, 0.95, ry);
            col *= lerp(float3(1.0, 1.0, 1.0),
                        sub * border * row_gap,
                        saturate(mask_amt));
            col *= 1.0 + saturate(mask_amt) * 0.9;
        } else {
            const float m = saturate(mask_amt);
            const float edge = 0.25 + 0.75 * dot(c, c);
            const float dc = converge * edge;   // px

            // The phosphor lattice uses faceplate space, not screen space.
            const float2 fp = suv * size;
            const float sw = max(triad, 2.0);

            int ch;
            float cov;
            float ex, ey;
            if (mm == 2) {
                const float rowf = fp.y / sw;
                const int row_i = int(floor(rowf));
                const float xoff = (row_i & 1) != 0 ? 0.5 : 0.0;
                const float colf = fp.x / sw + xoff;
                const int col_i = int(floor(colf));
                const float2 d = float2(frac(colf), frac(rowf)) - 0.5;
                cov = 1.0 - smoothstep(0.30, 0.52, sqrt(dot(d, d)));
                ch = ((col_i + row_i) % 3 + 3) % 3;
                ex = (float(col_i) + 0.5 - xoff) * sw;
                ey = (float(row_i) + 0.5) * sw;
            } else {
                const float px = fp.x / sw;
                const int elem = int(floor(px));
                ch = (elem % 3 + 3) % 3;
                const float in_e = frac(px);
                cov = smoothstep(0.0, 0.18, in_e) *
                      (1.0 - smoothstep(0.82, 1.0, in_e));
                ex = (float(elem) + 0.5) * sw;
                ey = fp.y;
                if (mm == 1) {
                    const float sh = sw * 2.5;
                    const float yoff =
                        (elem & 1) != 0 ? sh * 0.5 : 0.0;
                    const float ry = (fp.y + yoff) / sh;
                    const float in_y = frac(ry);
                    cov *= smoothstep(0.0, 0.10, in_y) *
                           (1.0 - smoothstep(0.90, 1.0, in_y));
                    ey = (floor(ry) + 0.5) * sh - yoff;
                }
            }

            const float bx = lerp(fp.x, ex, m);
            const float3 sx =
                float3(bx - dc, bx, bx + dc) / size.x;
            const float by = lerp(fp.y, ey, m);

            const float nlines = clamp(lines, 64.0, size.y);
            const float pitch = size.y / nlines;
            const float ys = by / pitch;
            const float l0 = floor(ys - 0.5) + 0.5;
            float3 lv0, lv1, w0, w1;
            float a0, a1;
            beam_line(sx, l0 * pitch / size.y, ys - l0, bloom,
                      lv0, w0, a0);
            beam_line(sx, (l0 + 1.0) * pitch / size.y, l0 + 1.0 - ys,
                      bloom, lv1, w1, a1);
            // The 1.25 factor replaces the energy the line integral loses.
            const float3 beam = (lv0 * w0 + lv1 * w1) * 1.25;

            const float4 fg = input.SampleLevel(float2(sx.y, suv.y), 0);
            const float fr =
                input.SampleLevel(float2(sx.x, suv.y), 0).r;
            const float fb =
                input.SampleLevel(float2(sx.z, suv.y), 0).b;
            col = lerp(float3(fr, fg.g, fb), beam, saturate(scanline));
            col_a = lerp(fg.a, max(a0, a1), saturate(scanline));

            float3 tr = float3(0.05, 0.05, 0.05);
            if (ch == 0) tr.r = max(cov, 0.05);
            else if (ch == 1) tr.g = max(cov, 0.05);
            else tr.b = max(cov, 0.05);
            col *= lerp(float3(1.0, 1.0, 1.0), tr, m);
            col *= 1.0 + m * 1.8;

            if (halation > 0.001) {
                const float rh = size.y * 0.035;
                float3 hal = float3(0.0, 0.0, 0.0);
                float hal_a = 0.0;
                [unroll]
                for (int i = 0; i < 8; ++i) {
                    const float a = float(i) * 0.7854 + 0.39;
                    const float2 p = clamp(
                        suv + float2(cos(a), sin(a)) * rh / size, 0.0, 1.0);
                    const float4 s = input.SampleLevel(p, 0);
                    hal += s.rgb;
                    hal_a += s.a;
                }
                hal *= 0.125;
                hal_a *= 0.125;
                const float3 h2 = hal * hal;
                col += h2 * halation * 0.45;
                col_a = max(col_a, saturate(fx_luma(h2)) * halation * 0.45 *
                                       hal_a);
            }

            const float corner =
                saturate(1.0 - dot(c, c) * 0.28 * curvature);
            col *= corner;
        }
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
