// Colors arrive sRGB-encoded from the UI and are decoded into the linear
// working space here.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint kind;         // LayerSourceKind: 1 solid, 2 gradient, 3 noise,
                       // 4 pattern, 5 oscillator, 6 shape
    uint seed;
    uint frame;
    float ar, ag, ab;  // sRGB
    float br, bg, bb;  // sRGB
    float scale;
    float angle;       // radians
    uint shape;        // noise: 0 value, 1 perlin, 2 cellular, 3 fbm,
                       // 4 static; pattern: 0 checker, 1 stripes,
                       // 2 grid, 3 dots, 4 smpte bars; oscillator:
                       // 0 sine bars, 1 rings, 2 plasma, 3 lissajous;
                       // shape node: 0 circle, 1 box, 2 diamond,
                       // 3 custom path (SDF texture)
    float phase;       // oscillator: percent of one period; wraps, so
                       // 0 and 100 are identical
};

// Custom-shape SDF (gfx/shape_sdf): canvas uv, 0.5 = on the curve,
// distances span +-0.25 canvas heights. Dummy-bound for every other
// generator kind.
[[vk::binding(0, 0)]] Sampler2D shape_sdf;

// Value noise at integer lattice p with smooth (quintic-free) fade.
float gen_value_noise(float2 p, uint seed, uint t)
{
    const int2 c = int2(floor(p));
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    const float n00 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4096), uint(c.y + 4096)));
    const float n10 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4097), uint(c.y + 4096)));
    const float n01 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4096), uint(c.y + 4097)));
    const float n11 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4097), uint(c.y + 4097)));
    return lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y);
}

// Perlin-style gradient noise: hashed unit gradients at the lattice,
// remapped to 0..1.
float gen_perlin(float2 p, uint seed, uint t)
{
    const int2 c = int2(floor(p));
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    float d[4];
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const int2 o = int2(int(i & 1u), int(i >> 1));
        const float a = fx_hash01(fx_hash4(seed, t, uint(c.x + o.x + 4096),
                                           uint(c.y + o.y + 4096))) *
                        6.2831853;
        const float2 g = float2(cos(a), sin(a));
        d[i] = dot(g, f - float2(o));
    }
    const float n =
        lerp(lerp(d[0], d[1], u.x), lerp(d[2], d[3], u.x), u.y);
    return saturate(0.5 + n * 0.7071);
}

// Worley/cellular: distance to the nearest seeded feature point in the
// 3x3 neighborhood, 0 at the point rising outward.
float gen_worley(float2 p, uint seed, uint t)
{
    const int2 c = int2(floor(p));
    const float2 f = frac(p);
    float md = 8.0;
    [unroll]
    for (int oy = -1; oy <= 1; ++oy)
        [unroll]
        for (int ox = -1; ox <= 1; ++ox)
        {
            const uint hx = fx_hash4(seed, t, uint(c.x + ox + 4096),
                                     uint(c.y + oy + 4096));
            const float2 fp = float2(ox, oy) +
                              float2(fx_hash01(hx),
                                     fx_hash01(fx_hash(hx))) -
                              f;
            md = min(md, dot(fp, fp));
        }
    return saturate(sqrt(md));
}

[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float3 color_a = fx_srgb_eotf(saturate(float3(ar, ag, ab)));
    const float3 color_b = fx_srgb_eotf(saturate(float3(br, bg, bb)));

    float3 rgb;
    float alpha = 1.0;
    if (kind == 2)
    {
        const float2 dir = float2(cos(angle), sin(angle));
        const float t = saturate(dot(uv - 0.5, dir) + 0.5);
        rgb = lerp(color_a, color_b, t);
    }
    else if (kind == 3)
    {
        // Noise family: value / perlin / cellular / fbm boil on a slow
        // frame tick; static re-rolls every frame per pixel.
        const float cell = max(scale, 2.0);
        const float2 p = float2(tid.xy) / cell;
        const uint t = frame / 4;
        float n;
        if (shape == 1u)
        {
            n = gen_perlin(p, seed, t);
        }
        else if (shape == 2u)
        {
            n = 1.0 - gen_worley(p, seed, t);
        }
        else if (shape == 3u)
        {
            n = 0.5000 * gen_value_noise(p, seed, t) +
                0.2500 * gen_value_noise(p * 2.0, fx_hash(seed), t) +
                0.1250 * gen_value_noise(p * 4.0, fx_hash(seed ^ 0x9E1u), t) +
                0.0625 * gen_value_noise(p * 8.0, fx_hash(seed ^ 0x3C7u), t);
            n = saturate(n * (1.0 / 0.9375));
        }
        else if (shape == 4u)
        {
            n = fx_hash01(fx_hash4(seed, frame, tid.x, tid.y));
        }
        else
        {
            n = gen_value_noise(p, seed, t);
        }
        rgb = lerp(color_b, color_a, n);
    }
    else if (kind == 4)
    {
        // Pattern family: two-color screens on a rotatable cell grid,
        // plus the SMPTE-style bars (which keep their fixed colors).
        if (shape == 4u)
        {
            // 75% SMPTE-style bars, grayscale ramp, pluge strip.
            if (uv.y < 0.72)
            {
                const int bar = min(int(uv.x * 7.0), 6);
                float3 bars[7] = {
                    float3(0.75, 0.75, 0.75), float3(0.75, 0.75, 0.0),
                    float3(0.0, 0.75, 0.75),  float3(0.0, 0.75, 0.0),
                    float3(0.75, 0.0, 0.75),  float3(0.75, 0.0, 0.0),
                    float3(0.0, 0.0, 0.75),
                };
                rgb = fx_srgb_eotf(bars[bar]);
            }
            else if (uv.y < 0.86)
            {
                rgb = fx_srgb_eotf(float3(uv.x, uv.x, uv.x));
            }
            else
            {
                const int seg = min(int(uv.x * 8.0), 7);
                const float v = (seg & 1) == 0 ? 0.0
                                : (seg < 4 ? 0.04 : (seg < 6 ? 0.5 : 1.0));
                rgb = fx_srgb_eotf(float3(v, v, v));
            }
        }
        else
        {
            const float cell = max(scale, 2.0);
            const float ca = cos(angle);
            const float sa = sin(angle);
            const float2 centered =
                float2(tid.xy) - float2(width, height) * 0.5;
            const float2 rp = float2(centered.x * ca - centered.y * sa,
                                     centered.x * sa + centered.y * ca) /
                              cell;
            float v;
            if (shape == 1u)
            {
                v = frac(rp.x) < 0.5 ? 1.0 : 0.0;          // stripes
            }
            else if (shape == 2u)
            {
                const float2 g = abs(frac(rp) - 0.5);      // grid lines
                v = min(g.x, g.y) < 0.06 ? 1.0 : 0.0;
            }
            else if (shape == 3u)
            {
                const float2 d = frac(rp) - 0.5;           // dot screen
                v = length(d) < 0.3 ? 1.0 : 0.0;
            }
            else
            {
                const int2 cp = int2(floor(rp));           // checker
                v = float((cp.x + cp.y) & 1);
            }
            rgb = lerp(color_b, color_a, v);
        }
    }
    else if (kind == 5)
    {
        // scale = cycles across the frame; motion creeps on the frame
        // clock, never wall clock. `phase` slides the pattern along its
        // own direction: 100% is one full period, so 0 and 100 match
        // exactly (every term shifts by a whole 2 pi).
        const float t = float(frame) * 0.02;
        const float ph = frac(phase * 0.01) * 6.2831853;
        const float freq = max(scale, 0.5) * 6.2831853;
        float v;
        if (shape == 1u)
        {
            const float2 d = (uv - 0.5) *
                             float2(float(width) / float(height), 1.0);
            v = 0.5 + 0.5 * sin(length(d) * freq - t * 2.0 - ph);
        }
        else if (shape == 2u)
        {
            const float2 d = uv * freq * 0.5;
            const float p = sin(d.x + t + ph) + sin(d.y - t * 0.7 - ph) +
                            sin((d.x + d.y) * 0.6 + t * 1.3 + ph) +
                            sin(length(uv - 0.5) * freq - t - ph);
            v = 0.5 + 0.25 * p * 0.5;
        }
        else if (shape == 3u)
        {
            // Distance field to sin(a*s), sin(b*s) with b = a + 1;
            // scale picks the ratio, angle the phase offset.
            const float2 asp =
                float2(float(width) / float(height), 1.0);
            const float2 p = (uv - 0.5) * asp;
            const float fa = clamp(floor(scale * 0.5 + 0.5), 1.0, 6.0);
            const float fb = fa + 1.0;
            // Polyline distance, not point distance: sampling the curve
            // as points alone leaves gaps between the samples.
            float md = 1e9;
            float2 prev = 0.42 * float2(sin(t * 0.7 + angle + ph),
                                        sin(t * 0.3));
            for (uint i = 1; i <= 128; ++i)
            {
                const float s = float(i) / 128.0 * 6.2831853;
                const float2 q = 0.42 *
                    float2(sin(fa * s + t * 0.7 + angle + ph),
                           sin(fb * s + t * 0.3));
                const float2 ab = q - prev;
                const float hseg = saturate(dot(p - prev, ab) /
                                            max(dot(ab, ab), 1e-8));
                md = min(md, length(p - (prev + ab * hseg)));
                prev = q;
            }
            v = exp(-pow(md / 0.004, 2.0)) +
                0.18 * exp(-pow(md / 0.02, 2.0));
            v = saturate(v);
        }
        else
        {
            const float2 dir = float2(cos(angle), sin(angle));
            v = 0.5 + 0.5 * sin(dot(uv - 0.5, dir) * freq + t + ph);
        }
        rgb = lerp(color_b, color_a, saturate(v));
    }
    else if (kind == 6)
    {
        // Centered SDF matte drawn color_a on color_b. angle carries the
        // feather width here, not an orientation; position and rotation
        // come from the layer transform instead.
        const float2 p = (uv - 0.5) *
                         float2(float(width) / float(height), 1.0);
        const float r = clamp(scale * 0.05, 0.02, 1.5);
        float d;
        if (shape == 3u)
        {
            // Custom path: the CPU raster's SDF, size scaling about the
            // canvas center (sample at uv/s, distance times s). scale 6
            // = drawn size.
            const float s = clamp(scale, 0.5, 30.0) * (1.0 / 6.0);
            const float2 suv = (uv - 0.5) / s + 0.5;
            d = (shape_sdf.SampleLevel(clamp(suv, 0.0, 1.0), 0).r - 0.5) *
                0.5 * s;
        }
        else if (shape == 1u)
            d = max(abs(p.x), abs(p.y)) - r;          // box
        else if (shape == 2u)
            d = (abs(p.x) + abs(p.y)) * 0.7071 - r;   // diamond
        else
            d = length(p) - r;                        // circle
        const float feather = max(abs(angle) * 0.25, 0.002);
        const float v = saturate(0.5 - d / feather);
        // Premultiplied: the SDF coverage IS the alpha. color_b is dead
        // for shapes - transparency replaced "draw on a background".
        const float sv = v * v * (3.0 - 2.0 * v);
        rgb = color_a * sv;
        alpha = sv;
    }
    else
    {
        rgb = color_a;
    }
    output[tid.xy] = float4(rgb, alpha);
}
