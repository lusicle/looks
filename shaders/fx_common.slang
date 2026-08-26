// Shared kernel helpers. All randomness here is counter-based hashing
// keyed on (seed, frame, cell): no stateful RNG anywhere, which is what
// keeps preview and export bit-exact.
//
// ALPHA IS THE GATE: the layer composite reveals a pixel only through
// its alpha (no emissive path), so every kernel must keep alpha equal
// to the COVERAGE of its output content. Remaps carry the sampled
// pixel's alpha, gathers gather alpha with the same weights, trails
// and added light extend it, burn-ins union it in, full-frame redraws
// own it; only pure color work keeps base.a. Footage is alpha-1
// everywhere and every rule above preserves constant 1, so this only
// matters - and matters completely - for shapes and mattes.

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

static const float3 kFxLumaW = float3(0.2126, 0.7152, 0.0722);

float fx_luma(float3 rgb)
{
    return dot(rgb, kFxLumaW);
}

// BT.709 chroma difference scales: Cb carries (B-Y)/kFxCb709, Cr carries
// (R-Y)/kFxCr709. Every encoder, decoder and scope derives from these two
// anchors plus the fx_luma weights so the round trip stays inverse.
static const float kFxCb709 = 1.8556;
static const float kFxCr709 = 1.5748;
static const float kFxCb709G = kFxCb709 * kFxLumaW.b / kFxLumaW.g;
static const float kFxCr709G = kFxCr709 * kFxLumaW.r / kFxLumaW.g;

// Encoded-domain RGB to (Cb, Cr) about the given luma.
float2 fx_rgb_to_cbcr709(float3 enc, float y)
{
    return float2((enc.b - y) / kFxCb709, (enc.r - y) / kFxCr709);
}

// Screen blend, bounded: inputs saturate so linear-light values above 1
// cannot flip the term negative and push the result past white.
float3 fx_screen(float3 a, float3 b)
{
    return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b));
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

// Fixed-palette snap shared by the Quantizer and PaletteMap: 0 DMG
// greens, 1 CGA, 2 NES, 3 teletext, 4 duotone. Luma palettes read l,
// color palettes read c_enc; callers prepare both (dither, cycling).
float3 fx_palette_snap(int pal, float3 c_enc, float l,
                       float hue_a, float hue_b)
{
    if (pal == 0)
        return fx_nearest_of4(float3(l, l, l),
                              float3(0.059, 0.220, 0.059),
                              float3(0.188, 0.384, 0.188),
                              float3(0.545, 0.675, 0.059),
                              float3(0.608, 0.737, 0.059));
    if (pal == 1)
        return fx_nearest_of4(c_enc,
                              float3(0.0, 0.0, 0.0),
                              float3(0.333, 1.0, 1.0),
                              float3(1.0, 0.333, 1.0),
                              float3(1.0, 1.0, 1.0));
    if (pal == 2)
        return fx_nes_nearest(c_enc);
    if (pal == 3)
        return round(c_enc);
    const float3 ha = fx_hue_to_rgb(hue_a);
    const float3 hb = fx_hue_to_rgb(hue_b);
    return fx_nearest_of4(float3(l, l, l),
                          float3(0.02, 0.02, 0.02),
                          ha * 0.45,
                          hb * 0.85,
                          float3(0.98, 0.98, 0.98));
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

// Bilinear 2D value noise on an integer lattice.
float fx_vnoise2(uint s, float2 p)
{
    const float2 i = floor(p);
    const float2 f = p - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const int2 ip = int2(i) + int2(4096, 4096);
    const float a = fx_hash01(fx_hash4(s, uint(ip.x), uint(ip.y), 0u));
    const float b = fx_hash01(fx_hash4(s, uint(ip.x + 1), uint(ip.y), 0u));
    const float c = fx_hash01(fx_hash4(s, uint(ip.x), uint(ip.y + 1), 0u));
    const float d = fx_hash01(fx_hash4(s, uint(ip.x + 1), uint(ip.y + 1),
                                       0u));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
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
