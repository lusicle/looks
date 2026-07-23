// Shared kernel helpers (spec §11): all randomness is a counter-based hash
// keyed on (seed, frame, cell) — no stateful RNG anywhere, so preview and
// export are bit-exact. Included by effect kernels via #include.

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

// Hue (0..1) -> fully saturated RGB (the abs-ramp construction).
float3 fx_hue_to_rgb(float h)
{
    const float r = abs(h * 6.0 - 3.0) - 1.0;
    const float g = 2.0 - abs(h * 6.0 - 2.0);
    const float b = 2.0 - abs(h * 6.0 - 4.0);
    return saturate(float3(r, g, b));
}

// sRGB transfer pair (display-referred effects convert around their core).
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
