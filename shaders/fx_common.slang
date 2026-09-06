// All randomness uses counter hashes on (seed, frame, cell). Keep it stateless.
// Every kernel must set alpha equal to the coverage of its output content.

#ifndef FX_COMMON_SLANG
#define FX_COMMON_SLANG

float fx_decay(float retention, float fps)
{
    return pow(saturate(retention), 60.0 / max(fps, 1.0));
}

float fx_sinc(float x)
{
    return abs(x) < 0.0001 ? 1.0 : sin(x) / x;
}

float fx_segment_distance(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    return length(p - a - ab * saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-8)));
}

float4 fx_sample_border(Sampler2D tex, float2 uv)
{
    return tex.SampleLevel(saturate(uv), 0) *
        float(all(uv >= 0.0) && all(uv <= 1.0));
}

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

// Cb carries (B-Y)/kFxCb709 and Cr carries (R-Y)/kFxCr709.
// All encoders and scopes must use these anchors to keep the round trip.
static const float kFxCb709 = 1.8556;
static const float kFxCr709 = 1.5748;
static const float kFxCb709G = kFxCb709 * kFxLumaW.b / kFxLumaW.g;
static const float kFxCr709G = kFxCr709 * kFxLumaW.r / kFxLumaW.g;

// The RGB input must be encoded, not linear.
float2 fx_rgb_to_cbcr709(float3 enc, float y)
{
    return float2((enc.b - y) / kFxCb709, (enc.r - y) / kFxCr709);
}

// Saturate the inputs. Linear values above 1 push the result past white.
float3 fx_screen(float3 a, float3 b)
{
    return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b));
}

// The hue input h is in the range 0 to 1.
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

float3 fx_straight(float4 c)
{
    return c.a > 1e-6 ? c.rgb / c.a : float3(0.0, 0.0, 0.0);
}

float3 fx_encoded(float4 c)
{
    return fx_srgb_oetf(saturate(fx_straight(c)));
}

float4 fx_from_encoded(float3 c, float alpha)
{
    return float4(fx_srgb_eotf(saturate(c)) * alpha, alpha);
}

float4 fx_blend(float4 a, float4 b, uint mode)
{
    const float alpha = a.a + b.a * (1.0 - a.a);
    if (mode == 0u) return float4(b.rgb + a.rgb * (1.0 - b.a), alpha);
    const float3 ca = fx_straight(a);
    const float3 cb = fx_straight(b);
    float3 mixed = cb;
    if (mode == 1u) mixed = ca + cb;
    else if (mode == 2u) mixed = ca * cb;
    else if (mode == 3u) mixed = fx_screen(ca, cb);
    else if (mode == 4u) mixed = abs(ca - cb);
    else if (mode == 5u) mixed = max(ca - cb, 0.0);
    else if (mode == 6u) mixed = min(ca, cb);
    else if (mode == 7u) mixed = max(ca, cb);
    else if (mode == 8u) mixed = lerp(2.0 * ca * cb,
        1.0 - 2.0 * (1.0 - ca) * (1.0 - cb), step(0.5, ca));
    return float4(a.rgb * (1.0 - b.a) + b.rgb * (1.0 - a.a) +
                  mixed * (a.a * b.a), alpha);
}

// The packed LUT is 512x192 R8. Rows 0 to 63 hold the STBN 64x64x8 atlas,
// slice s at x = s*64. Rows 64 to 191 hold the 128x128 blue noise.

static const float kFxBayer2[4] = {0.0, 2.0, 3.0, 1.0};
static const float kFxBayer4[16] = {
    0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
    3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0};

float fx_bayer8(uint2 p)
{
    const uint2 q = p & 7;
    const float base = kFxBayer4[(q.y & 3) * 4 + (q.x & 3)];
    const float quad = float(((q.y >> 2) & 1) * 2 + ((q.x >> 2) & 1));
    return (base * 4.0 + kFxBayer2[uint(quad)]) / 64.0;
}

// Clustered-dot screen at 45 degrees on an 8 px lattice.
float fx_cluster_dot(float2 pf)
{
    const float2 r45 = float2(pf.x - pf.y, pf.x + pf.y) * 0.70710678;
    const float2 cell = frac(r45 * 0.125) * 8.0 - 4.0;
    return saturate(1.0 - dot(cell, cell) * 0.04908739);
}

// p is a screen-space pixel coordinate.
float fx_ign(uint2 p)
{
    const float g = 0.06711056 * float(p.x) + 0.00583715 * float(p.y);
    return frac(52.9829189 * frac(g));
}

float fx_enc_luma(Sampler2D tex, float2 uv)
{
    return fx_luma(fx_encoded(tex.SampleLevel(uv, 0)));
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

// The caller must supply both l and c_enc. Each palette reads only one.
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

// The linear interpolant is the probability, so the dither averages back.
// The function returns the level index, not a color.
float3 fx_dither_levels(float3 lin, float n, float t01)
{
    lin = saturate(lin);
    const float3 enc = fx_srgb_oetf(lin);
    const float3 lo = clamp(floor(enc * (n - 1.0)), 0.0, n - 2.0);
    const float3 lin_lo = fx_srgb_eotf(lo / (n - 1.0));
    const float3 lin_hi = fx_srgb_eotf((lo + 1.0) / (n - 1.0));
    const float3 p =
        (lin - lin_lo) / max(lin_hi - lin_lo, 1.0e-6);
    // Use a strict compare. A pixel on a level must never move up.
    return lo + step(t01 + 1.0e-6, p);
}

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
