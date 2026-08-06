// Colors arrive sRGB-encoded from the UI and are decoded into the linear
// working space here.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint kind;         // LayerSourceKind: 1 solid, 2 gradient, 3 noise,
                       // 4 test pattern, 5 oscillator, 6 shape
    uint seed;
    uint frame;
    float ar, ag, ab;  // sRGB
    float br, bg, bb;  // sRGB
    float scale;
    float angle;       // radians
    uint shape;        // oscillator: 0 sine bars, 1 rings, 2 plasma,
                       // 3 lissajous; shape node: 0 circle, 1 box,
                       // 2 diamond
};

[[vk::binding(0, 0)]] RWTexture2D<float4> output;

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
        const float cell = max(scale, 2.0);
        const float2 p = float2(tid.xy) / cell;
        const int2 c = int2(floor(p));
        const float2 f = frac(p);
        const float2 u = f * f * (3.0 - 2.0 * f);
        const uint t = frame / 4;
        const float n00 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4096), uint(c.y + 4096)));
        const float n10 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4097), uint(c.y + 4096)));
        const float n01 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4096), uint(c.y + 4097)));
        const float n11 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4097), uint(c.y + 4097)));
        const float n = lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y);
        rgb = lerp(color_b, color_a, n);
    }
    else if (kind == 4)
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
    else if (kind == 5)
    {
        // scale = cycles across the frame; phase creeps on the frame
        // clock, never wall clock.
        const float t = float(frame) * 0.02;
        const float freq = max(scale, 0.5) * 6.2831853;
        float v;
        if (shape == 1u)
        {
            const float2 d = (uv - 0.5) *
                             float2(float(width) / float(height), 1.0);
            v = 0.5 + 0.5 * sin(length(d) * freq - t * 2.0);
        }
        else if (shape == 2u)
        {
            const float2 d = uv * freq * 0.5;
            const float p = sin(d.x + t) + sin(d.y - t * 0.7) +
                            sin((d.x + d.y) * 0.6 + t * 1.3) +
                            sin(length(uv - 0.5) * freq - t);
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
            float2 prev = 0.42 * float2(sin(t * 0.7 + angle),
                                        sin(t * 0.3));
            for (uint i = 1; i <= 128; ++i)
            {
                const float s = float(i) / 128.0 * 6.2831853;
                const float2 q = 0.42 *
                    float2(sin(fa * s + t * 0.7 + angle),
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
            v = 0.5 + 0.5 * sin(dot(uv - 0.5, dir) * freq + t);
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
        if (shape == 1u)
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
