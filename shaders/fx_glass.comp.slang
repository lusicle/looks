#include "fx_common.slang"

// Architectural glass as ONE optical pipeline over per-profile surface
// slope fields. A profile only answers "what is the surface gradient
// here"; everything optical is shared and derives from that gradient:
// refraction (surface angle via atan, exit travel via tan - gentle at
// flat glass, folding at steep glass, so interiors warp instead of
// shifting whole cells), chromatic dispersion along the bend, Fresnel
// transmission loss where the surface turns away, relief lighting, and
// an environment-lit specular that breaks up along the surface. Adding
// a profile means adding a slope function, nothing else.
// Glass is static: nothing here reads the frame clock.

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

// Smoothed 2D value noise in [-1, 1], one octave.
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

// Cylinder cross-section slope: h = sqrt(1 - n^2) has dh/dn =
// -n/sqrt(1 - n^2), near-linear at the crown and asymptotic at the
// edge - the interior nonlinearity fluted glass shows.
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
    const float2 p = float2(tid.xy) + 0.5;

    // ---- profile: surface slope field + cell bookkeeping
    float2 g = float2(0.0, 0.0);   // surface gradient (unitless slope)
    float seam = 0.0;              // grout line weight, cell types only
    float2 nv = float2(0.0, 0.0);  // low-frequency breakup

    if (ty == 2) {
        // Rolled antique glass: crossed low-frequency waves whose
        // phases wander with noise - straight periodic crests would
        // fire the specular on a dot lattice instead of streaks.
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
        // Frosted: the noise vector IS the slope; wobble stretches the
        // grain into brushed streaks.
        const float2 np = p / float2(cw, cw * (1.0 + wb * 2.0));
        g = glass_noise2(np, seed) * 1.6;
        nv = glass_noise2(p / (cw * 2.5), fx_hash(seed ^ 0x517Bu));
    } else {
        // Cell profiles. Wobble bends the cell axes first so flutes and
        // courses stop being ruler-straight.
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
            // Fluted / domed: cylinder slope per axis.
            g.x = glass_cyl_slope(local.x * 2.0);
            if (cells_y) g.y = glass_cyl_slope(local.y * 2.0);
        } else if (ty == 1) {
            // Prism: flat triangular facets - constant slope per
            // facet, so each facet displaces uniformly and flips hard
            // at the ridge (that IS the prism look; curvature would
            // make it a lens).
            g.x = (local.x < 0.0 ? -1.6 : 1.6);
            if (cells_y) g.y = (local.y < 0.0 ? -1.6 : 1.6);
        } else {
            // Hammered: one seeded pit or mound per cell, center
            // jittered by wobble; the dome gradient is the slope.
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

    // ---- shared optics, all from the slope field
    const float slen = length(g);
    const float2 gdir = slen > 1.0e-4 ? g / slen : float2(1.0, 0.0);
    const float3 nrm = normalize(float3(-g, 1.0));

    // Refraction: surface angle via atan, deviation scaled by the
    // refract knob (an ior proxy), exit travel via tan. Glass depth
    // rides the cell size, so coarse glass bends farther.
    const float dev = atan(slen) * (0.12 + 0.88 * rf);
    const float travel = tan(min(dev, 1.25)) * cw * 0.45;
    const float2 disp = -gdir * travel;
    const float2 duv = disp / size;

    // Soften: blur along the bend, growing where the surface is steep;
    // frosted and rolled glass diffuse over their whole face.
    const float sf = saturate(soften);
    const float blur_px =
        celled ? sf * cw * (0.06 + 0.12 * saturate(slen * 0.4))
               : sf * cw * 0.4;
    const float2 buv = gdir * blur_px / size;

    // Dispersion splits the channels along the bend.
    const float dsp = saturate(dispersion) * 0.3;
    const float2 uv_g = clamp(uv + duv, 0.0, 1.0);
    const float2 uv_r = clamp(uv + duv * (1.0 - dsp), 0.0, 1.0);
    const float2 uv_b = clamp(uv + duv * (1.0 + dsp), 0.0, 1.0);
    float3 col;
    const float4 sr = input.SampleLevel(uv_r, 0);
    const float4 sg = input.SampleLevel(uv_g, 0);
    const float4 sb = input.SampleLevel(uv_b, 0);
    col.r = sr.r;
    col.g = sg.g;
    col.b = sb.b;
    // Refracted coverage: the union of the split rays - the glass moves
    // the content, so its alpha moves with it.
    float col_a = max(sg.a, max(sr.a, sb.a));
    const float4 s0 = input.SampleLevel(clamp(uv_g - buv, 0.0, 1.0), 0);
    const float4 s1 = input.SampleLevel(clamp(uv_g + buv, 0.0, 1.0), 0);
    col = lerp(col, (col + s0.rgb + s1.rgb) * (1.0 / 3.0),
               saturate(blur_px * 0.6));
    col_a = lerp(col_a, (col_a + s0.a + s1.a) * (1.0 / 3.0),
                 saturate(blur_px * 0.6));

    // Fresnel transmission loss + relief lighting + grout, one depth
    // knob: steep glass passes less light and every facet takes the
    // fixed top-left key differently.
    const float ed = saturate(edge);
    const float fres = pow(1.0 - saturate(nrm.z), 5.0);
    const float3 key = normalize(float3(-0.45, -0.6, 0.66));
    const float relief = dot(nrm, key) - 0.66;
    col *= 1.0 - fres * ed * 0.85;
    col *= saturate(1.0 + relief * ed * 0.9);
    col *= 1.0 - seam * ed * 0.55;

    // Specular: Blinn lobe off the same normal, Fresnel-weighted,
    // responding to what is behind the glass and breaking up along the
    // surface - a constant band reads as plastic.
    const float3 half_v = normalize(key + float3(0.0, 0.0, 1.0));
    const float lobe = pow(saturate(dot(nrm, half_v)), 60.0);
    const float grazing = 0.25 + 0.75 * fres;
    const float env = 0.1 + 1.9 * fx_luma(col);
    const float amp = 0.35 + 0.65 * saturate(nv.x * 0.5 + 0.5);
    col += lobe * grazing * env * amp * saturate(glint) * 1.6;

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
