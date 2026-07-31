// Layer generators (spec §5): solid / gradient / value noise, rendered
// straight into the linear working space. Colors arrive sRGB-encoded from
// the UI and are decoded here. Noise is seeded + frame-animated,
// deterministic (spec §11).

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint kind;         // 1 solid, 2 gradient, 3 noise, 4 test pattern,
                       // 5 oscillator, 7 shape (LayerSourceKind)
    uint seed;
    uint frame;
    float ar, ag, ab;  // color A (sRGB)
    float br, bg, bb;  // color B (sRGB)
    float scale;       // noise cell size / oscillator frequency
    float angle;       // gradient / oscillator orientation, radians
    uint shape;        // oscillator: 0 sine bars, 1 rings, 2 plasma,
                       // 3 lissajous XY trace
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
    if (kind == 2)
    {
        const float2 dir = float2(cos(angle), sin(angle));
        const float t = saturate(dot(uv - 0.5, dir) + 0.5);
        rgb = lerp(color_a, color_b, t);
    }
    else if (kind == 3)
    {
        // Smooth 2D value noise, re-seeded cell lattice drifting per frame.
        const float cell = max(scale, 2.0);
        const float2 p = float2(tid.xy) / cell;
        const int2 c = int2(floor(p));
        const float2 f = frac(p);
        const float2 u = f * f * (3.0 - 2.0 * f);
        const uint t = frame / 4;   // gentle boil
        const float n00 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4096), uint(c.y + 4096)));
        const float n10 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4097), uint(c.y + 4096)));
        const float n01 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4096), uint(c.y + 4097)));
        const float n11 = fx_hash01(fx_hash4(seed, t, uint(c.x + 4097), uint(c.y + 4097)));
        const float n = lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y);
        rgb = lerp(color_b, color_a, n);
    }
    else if (kind == 4)
    {
        // Test pattern (spec §5): 75% SMPTE-style bars over a grayscale
        // ramp footer with a black/white pluge strip. Static (spec §11:
        // nothing here depends on wall clock).
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
        // Oscillator (video-synth source): a periodic pattern to feed
        // Displace / FM / ring-mod chains. `scale` = cycles across the
        // frame, `angle` orients it; phase creeps on the frame clock so
        // patched displacement breathes. Deterministic (frame-clocked).
        const float t = float(frame) * 0.02;
        const float freq = max(scale, 0.5) * 6.2831853;
        float v;
        if (shape == 1u)
        {
            // Concentric rings from center.
            const float2 d = (uv - 0.5) *
                             float2(float(width) / float(height), 1.0);
            v = 0.5 + 0.5 * sin(length(d) * freq - t * 2.0);
        }
        else if (shape == 2u)
        {
            // Plasma: three drifting sine fields interfering.
            const float2 d = uv * freq * 0.5;
            const float p = sin(d.x + t) + sin(d.y - t * 0.7) +
                            sin((d.x + d.y) * 0.6 + t * 1.3) +
                            sin(length(uv - 0.5) * freq - t);
            v = 0.5 + 0.25 * p * 0.5;
        }
        else if (shape == 3u)
        {
            // Lissajous XY trace (scope art): distance field to the
            // curve sin(a*s), sin(b*s) with b = a + 1, phase creeping on
            // the frame clock. `scale` picks the ratio, `angle` the
            // phase offset.
            const float2 asp =
                float2(float(width) / float(height), 1.0);
            const float2 p = (uv - 0.5) * asp;
            const float fa = clamp(floor(scale * 0.5 + 0.5), 1.0, 6.0);
            const float fb = fa + 1.0;
            // Distance to the polyline through 128 curve samples (point
            // distance alone leaves gaps between samples).
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
            // Sine bars along `angle`.
            const float2 dir = float2(cos(angle), sin(angle));
            v = 0.5 + 0.5 * sin(dot(uv - 0.5, dir) * freq + t);
        }
        rgb = lerp(color_b, color_a, saturate(v));
    }
    else if (kind == 7)
    {
        // Shape node (docs/flow_canvas.md v5.2): a centered SDF matte —
        // circle / box / diamond via `shape` — drawn color_a on color_b
        // (white-on-black by default). `scale` sizes it, `angle` is the
        // feather width; position/aspect/rotation come from the layer
        // transform, richer edges from the effect chain it feeds.
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
        rgb = lerp(color_b, color_a, v * v * (3.0 - 2.0 * v));
    }
    else
    {
        rgb = color_a;
    }
    output[tid.xy] = float4(rgb, 1.0);
}
