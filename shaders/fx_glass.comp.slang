#include "fx_common.slang"

// Glass is static. Do not read the frame clock.

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
    float type;
    float cell_w;
    float cell_h;
    float refract;
    float dispersion;
    float soften;
    float edge;
    float glint;
    float wobble;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// The result is in the range -1 to 1.
float2 glass_noise2(float2 p, uint s)
{
    const float2 c = floor(p);
    const float2 f = p - c;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const uint xi = uint(int(c.x) + 4096);
    const uint yi = uint(int(c.y) + 4096);
    float2 n00 = float2(fx_hash01(fx_hash4(s, xi, yi, 0u)),
                        fx_hash01(fx_hash4(s, xi, yi, 1u)));
    float2 n10 = float2(fx_hash01(fx_hash4(s, xi + 1u, yi, 0u)),
                        fx_hash01(fx_hash4(s, xi + 1u, yi, 1u)));
    float2 n01 = float2(fx_hash01(fx_hash4(s, xi, yi + 1u, 0u)),
                        fx_hash01(fx_hash4(s, xi, yi + 1u, 1u)));
    float2 n11 = float2(fx_hash01(fx_hash4(s, xi + 1u, yi + 1u, 0u)),
                        fx_hash01(fx_hash4(s, xi + 1u, yi + 1u, 1u)));
    const float2 nx0 = lerp(n00, n10, u.x);
    const float2 nx1 = lerp(n01, n11, u.x);
    return lerp(nx0, nx1, u.y) * 2.0 - 1.0;
}

float glass_cyl_slope(float n)
{
    const float c = clamp(n, -0.985, 0.985);
    return c / sqrt(1.0 - c * c);
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

    const int ty = int(type + 0.5);
    const float cw = max(cell_w, 6.0);
    const bool cells_y = cell_h >= 6.0;
    const float ch = cells_y ? cell_h : cw;
    const bool celled = ty == 0 || ty == 1 || ty == 3;
    const float rf = saturate(refract);
    const float wb = saturate(wobble);
    const float2 p = uv * float2(canvas_w, canvas_h);

    float2 g = float2(0.0, 0.0);   // surface gradient (unitless slope)
    float seam = 0.0;
    float2 nv = float2(0.0, 0.0);

    if (ty == 2) {
        const float ph0 = fx_hash01(fx_hash3(seed, 11u, 0u)) * 6.2832;
        const float ph1 = fx_hash01(fx_hash3(seed, 13u, 0u)) * 6.2832;
        const float2 wn =
            glass_noise2(p / (cw * 6.0), fx_hash(seed ^ 0x2Fu)) * 2.2;
        const float wx = 6.2832 / (cw * 3.0);
        const float wy = 6.2832 / (cw * 4.7);
        g.x = cos(p.x * wx + ph0 + wn.x) * 1.1;
        g.y = cos(p.y * wy + ph1 + wn.y) * 1.1 * (0.3 + wb);
        nv = glass_noise2(p / (cw * 2.5), fx_hash(seed ^ 0x517Bu));
    } else if (ty == 4) {
        const float2 np = p / float2(cw, cw * (1.0 + wb * 2.0));
        g = glass_noise2(np, seed) * 1.6;
        nv = glass_noise2(p / (cw * 2.5), fx_hash(seed ^ 0x517Bu));
    } else {
        float2 pw = p;
        pw.x += sin(p.y * 6.2832 / (cw * 9.0)) * wb * cw * 0.25;
        if (cells_y)
            pw.y += sin(p.x * 6.2832 / (ch * 9.0)) * wb * ch * 0.25;
        const float2 cs = float2(cw, ch);
        const float2 ci = floor(pw / cs);
        const float2 local = (pw - (ci + 0.5) * cs) / cs;  // -0.5..0.5
        const float2 np =
            float2(dot(ci, float2(1.0, 57.0)) * 0.113,
                   cells_y ? dot(local, float2(1.3, 1.7)) * 1.5 +
                                 dot(ci, float2(0.7, 0.31))
                           : pw.y / (cs.x * 2.2));
        nv = glass_noise2(np, fx_hash(seed ^ 0x517Bu));

        if (ty == 0) {
            g.x = glass_cyl_slope(local.x * 2.0);
            if (cells_y) g.y = glass_cyl_slope(local.y * 2.0);
        } else if (ty == 1) {
            // The ridge flip is intended.
            g.x = (local.x < 0.0 ? -1.6 : 1.6);
            if (cells_y) g.y = (local.y < 0.0 ? -1.6 : 1.6);
        } else {
            const uint hx = uint(int(ci.x) + 4096);
            const uint hy = uint(int(ci.y) + 4096);
            const float2 jit =
                float2(fx_hash01(fx_hash4(seed, hx, hy, 2u)),
                       fx_hash01(fx_hash4(seed, hx, hy, 3u))) -
                0.5;
            const float sgn =
                fx_hash01(fx_hash4(seed, hx, hy, 4u)) < 0.5 ? -1.0
                                                            : 1.0;
            const float2 d = local - jit * wb * 0.6;
            const float r2 = saturate(dot(d, d) * 4.0);
            g = d * 8.0 * (1.0 - r2) * sgn;
        }
        seam = smoothstep(0.46, 0.5, max(abs(local.x),
                                         cells_y ? abs(local.y) : 0.0));
    }

    const float slen = length(g);
    const float2 gdir = slen > 1.0e-4 ? g / slen : float2(1.0, 0.0);
    const float3 nrm = normalize(float3(-g, 1.0));

    const float ior = 1.0 + rf * 0.7;
    const float incidence = atan(slen);
    const float dev = incidence - asin(sin(incidence) / ior);
    const float travel = tan(min(dev, 1.25)) * cw * 0.45;
    const float2 disp = -gdir * travel;
    const float2 duv = disp / float2(canvas_w, canvas_h);

    const float sf = saturate(soften);
    const float blur_px =
        celled ? sf * cw * (0.06 + 0.12 * saturate(slen * 0.4))
               : sf * cw * 0.4;

    const float dsp = saturate(dispersion) * 0.3;
    const float2 uv_g = uv + duv;
    const float2 uv_r = uv + duv * (1.0 - dsp);
    const float2 uv_b = uv + duv * (1.0 + dsp);
    float3 col;
    const float4 sr = fx_sample_border(input, uv_r);
    const float4 sg = fx_sample_border(input, uv_g);
    const float4 sb = fx_sample_border(input, uv_b);
    col.r = sr.r;
    col.g = sg.g;
    col.b = sb.b;
    // Alpha moves with the refracted content. Union the split rays.
    float col_a = max(sg.a, max(sr.a, sb.a));
    if (blur_px > 0.01) {
        float4 rough = 0.0;
        for (int i = 0; i < 16; ++i) {
            float theta = float(i) * 2.3999632;
            float2 offset = float2(cos(theta), sin(theta)) *
                sqrt((float(i) + 0.5) / 16.0) * blur_px / float2(canvas_w, canvas_h);
            rough += fx_sample_border(input, uv_g + offset);
        }
        col = lerp(col, rough.rgb / 16.0, saturate(blur_px * 0.6));
        col_a = lerp(col_a, rough.a / 16.0, saturate(blur_px * 0.6));
    }

    const float ed = saturate(edge);
    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fres = f0 + (1.0 - f0) * pow(1.0 - saturate(nrm.z), 5.0);
    const float3 key = normalize(float3(-0.45, -0.6, 0.66));
    const float relief = dot(nrm, key) - 0.66;
    col *= 1.0 - fres * ed * 0.85;
    col *= saturate(1.0 + relief * ed * 0.9);
    col *= 1.0 - seam * ed * 0.55;

    // Keep the specular breakup. A constant band looks like plastic.
    const float3 half_v = normalize(key + float3(0.0, 0.0, 1.0));
    const float lobe = pow(saturate(dot(nrm, half_v)), 60.0);
    const float grazing = 0.25 + 0.75 * fres;
    const float env = 0.1 * col_a + 1.9 * fx_luma(col);
    const float amp = 0.35 + 0.65 * saturate(nv.x * 0.5 + 0.5);
    col += lobe * grazing * env * amp * saturate(glint) * 1.6;

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
