// Shared kernel helpers. All randomness here is counter-based hashing
// keyed on (seed, frame, cell): no stateful RNG anywhere, which is what
// keeps preview and export bit-exact.

#ifndef FX_COMMON_SLANG
#define FX_COMMON_SLANG

// splitmix32-style avalanche.
uint fx_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint fx_hash3(uint seed, uint a, uint b)
{
    return fx_hash(seed ^ fx_hash(a ^ fx_hash(b)));
}

uint fx_hash4(uint seed, uint a, uint b, uint c)
{
    return fx_hash(seed ^ fx_hash(a ^ fx_hash(b ^ fx_hash(c))));
}

// Uniform [0, 1).
float fx_hash01(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

float fx_luma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

// Hue (0..1) -> fully saturated RGB, abs-ramp construction.
float3 fx_hue_to_rgb(float h)
{
    const float r = abs(h * 6.0 - 3.0) - 1.0;
    const float g = 2.0 - abs(h * 6.0 - 2.0);
    const float b = 2.0 - abs(h * 6.0 - 4.0);
    return saturate(float3(r, g, b));
}

float3 fx_srgb_oetf(float3 lin)
{
    const float3 hi = 1.055 * pow(max(lin, 0.0), 1.0 / 2.4) - 0.055;
    const float3 lo = lin * 12.92;
    return lerp(hi, lo, step(lin, 0.0031308));
}

float3 fx_srgb_eotf(float3 encoded)
{
    const float3 hi = pow((encoded + 0.055) / 1.055, 2.4);
    const float3 lo = encoded / 12.92;
    return lerp(hi, lo, step(encoded, 0.04045));
}

// The packed LUT is 512x192 R8: rows 0..63 hold the STBN 64x64x8 atlas
// with slice s at x = s*64, rows 64..191 the 128x128 blue noise.

static const float kFxBayer2[4] = {0.0, 2.0, 3.0, 1.0};
static const float kFxBayer4[16] = {
    0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
    3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0};

float fx_bayer8(uint2 p)
{
    // 8x8 from the recursive construction over the 4x4.
    const uint2 q = p & 7;
    const float base = kFxBayer4[(q.y & 3) * 4 + (q.x & 3)];
    const float quad = float(((q.y >> 2) & 1) * 2 + ((q.x >> 2) & 1));
    return (base * 4.0 + kFxBayer2[uint(quad)]) / 64.0;
}

// Clustered-dot screen threshold at 45 degrees: the black dot grows
// from the cell center of an 8 px lattice, and the radial map keeps dot
// area tracking tone (dots merge into the corners in the shadows).
float fx_cluster_dot(float2 pf)
{
    const float2 r45 = float2(pf.x - pf.y, pf.x + pf.y) * 0.70710678;
    const float2 cell = frac(r45 * 0.125) * 8.0 - 4.0;
    return saturate(1.0 - dot(cell, cell) * 0.04908739);
}

// Interleaved gradient noise: a screen-space ordered pattern between
// bayer's regularity and white noise's grain.
float fx_ign(uint2 p)
{
    const float g = 0.06711056 * float(p.x) + 0.00583715 * float(p.y);
    return frac(52.9829189 * frac(g));
}

float fx_lut_texel(Sampler2D lut, uint x, uint y)
{
    const float2 uv = (float2(float(x), float(y)) + 0.5) /
                      float2(512.0, 192.0);
    return lut.SampleLevel(uv, 0).r;
}

float fx_blue_noise(Sampler2D lut, uint2 p)
{
    return fx_lut_texel(lut, p.x & 127u, 64u + (p.y & 127u));
}

float fx_stbn(Sampler2D lut, uint2 p, uint slice)
{
    return fx_lut_texel(lut, (slice & 7u) * 64u + (p.x & 63u), p.y & 63u);
}

float3 fx_nearest_of4(float3 c, float3 p0, float3 p1, float3 p2, float3 p3)
{
    float3 best = p0;
    float best_d = dot(c - p0, c - p0);
    float d = dot(c - p1, c - p1);
    if (d < best_d) { best_d = d; best = p1; }
    d = dot(c - p2, c - p2);
    if (d < best_d) { best_d = d; best = p2; }
    d = dot(c - p3, c - p3);
    if (d < best_d) { best_d = d; best = p3; }
    return best;
}

// NES: a representative 12-entry cut of the 2C02 palette.
float3 fx_nes_nearest(float3 c)
{
    const float3 nes[12] = {
        float3(0.0, 0.0, 0.0),       float3(0.33, 0.33, 0.33),
        float3(0.74, 0.74, 0.74),    float3(1.0, 1.0, 1.0),
        float3(0.0, 0.47, 0.97),     float3(0.15, 0.15, 0.65),
        float3(0.9, 0.2, 0.16),      float3(0.65, 0.06, 0.0),
        float3(0.0, 0.66, 0.14),     float3(0.98, 0.7, 0.0),
        float3(0.96, 0.46, 0.7),     float3(0.35, 0.2, 0.7),
    };
    float3 best = nes[0];
    float best_d = 1.0e9;
    for (int i = 0; i < 12; ++i) {
        const float d = dot(c - nes[i], c - nes[i]);
        if (d < best_d) { best_d = d; best = nes[i]; }
    }
    return best;
}

// Picks between the two encoded levels bracketing lin using the LINEAR
// interpolant as the probability, so a dithered field averages back to
// the input on screen. Dithering in code values instead preserves the
// encoded mean, and the display's convex EOTF then lifts every dithered
// region: a 5% linear sky encodes near 0.25 and renders 25% white.
// Returns the level index, not a color.
float3 fx_dither_levels(float3 lin, float n, float t01)
{
    lin = saturate(lin);
    const float3 enc = fx_srgb_oetf(lin);
    const float3 lo = clamp(floor(enc * (n - 1.0)), 0.0, n - 2.0);
    const float3 lin_lo = fx_srgb_eotf(lo / (n - 1.0));
    const float3 lin_hi = fx_srgb_eotf((lo + 1.0) / (n - 1.0));
    const float3 p =
        (lin - lin_lo) / max(lin_hi - lin_lo, 1.0e-6);
    // Strict compare: p = 0 (a pixel ON a level) must never pick up.
    return lo + step(t01 + 1.0e-6, p);
}

// Smooth value noise over one dimension (deterministic wander).
float fx_value_noise1(uint seed, float t)
{
    const float cell = floor(t);
    const float frac = t - cell;
    const uint i = uint(int(cell) + 1000000);
    const float a = fx_hash01(fx_hash3(seed, i, 0u));
    const float b = fx_hash01(fx_hash3(seed, i + 1u, 0u));
    const float u = frac * frac * (3.0 - 2.0 * frac);
    return a + (b - a) * u;
}

#endif
