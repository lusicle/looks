#ifndef FX_PATTERN_SLANG
#define FX_PATTERN_SLANG

#include "fx_common.slang"

uint2 fx_pattern_shift(uint seed, uint boil_tick)
{
    return uint2(fx_hash3(seed, boil_tick, 11u) & 127u,
                 fx_hash3(seed, boil_tick, 13u) & 127u);
}

// mv is the flow advection in pixels. It is zero when screen-locked.
float2 fx_pattern_coords(uint2 pix, float2 mv, float pat_rotate,
                         float pat_scale, uint2 shift, float t,
                         float scroll, float warp)
{
    float2 pf = float2(pix) - mv;
    if (abs(pat_rotate) > 0.01) {
        const float pr = pat_rotate * 0.01745329252;
        const float prc = cos(pr), prs = sin(pr);
        pf = float2(pf.x * prc - pf.y * prs, pf.x * prs + pf.y * prc);
    }
    // The scale must floor at 1. A stride below 1 aliases the matrix.
    pf /= clamp(pat_scale, 1.0, 4.0);
    pf += float2(shift);
    pf += t * scroll * float2(1.0, 0.37);
    if (warp > 0.001) {
        pf += warp * 20.0 *
              float2(sin(pf.y * 0.043 + t * 0.9),
                     sin(pf.x * 0.037 - t * 0.7));
    }
    return pf;
}

float fx_ordered_pattern(int id, float2 pf, uint2 pix, uint seed,
                         uint boil_tick, float t, float2 dims,
                         Sampler2D noise_lut)
{
    const uint2 p = uint2(int2(floor(pf)) & 0x7FFF);
    if (id == 0)
        return (kFxBayer2[(p.y & 1) * 2 + (p.x & 1)] + 0.5) / 4.0;
    if (id == 1)
        return (kFxBayer4[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
    if (id == 2)
        return fx_bayer8(p) + 0.5 / 64.0;
    if (id == 3)
        return fx_hash01(fx_hash4(seed, boil_tick, pix.x, pix.y));
    if (id == 4)
        return fx_blue_noise(noise_lut, p);
    if (id == 5)
        return fx_stbn(noise_lut, p, boil_tick);
    if (id == 6) {
        const float ang = t * 0.15;
        const float ca = cos(ang), sa = sin(ang);
        const float2 r1 = float2(pf.x * ca - pf.y * sa,
                                 pf.x * sa + pf.y * ca);
        const float2 r2 = pf * 1.09;
        const float m = fx_bayer8(uint2(int2(floor(r1)) & 0x7FFF)) +
                        fx_bayer8(uint2(int2(floor(r2)) & 0x7FFF));
        return frac(m);
    }
    if (id == 7) {
        const float2 d = pf - dims * 0.5;
        return frac(length(d) * 0.125 - atan2(d.y, d.x) * 0.15915494);
    }
    if (id == 8) {
        const float2 d = pf - dims * 0.5;
        return frac(length(d) * 0.125);
    }
    if (id == 9) {
        const float2 f = frac(pf * 0.125) - 0.5;
        return saturate((abs(f.x) + abs(f.y)) * 1.4);
    }
    if (id == 10)
        return fx_cluster_dot(pf);
    if (id == 11)
        return (float(p.y & 3) + 0.5) * 0.25;
    if (id == 12)
        return ((p.x ^ p.y) & 1u) != 0u ? 0.75 : 0.25;
    if (id == 13)
        return fx_ign(p);
    return 0.5;
}

#endif
