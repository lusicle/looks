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
    uint kind;         // 1 solid, 2 gradient, 3 noise, 4 test pattern
                       // (LayerSourceKind)
    uint seed;
    uint frame;
    float ar, ag, ab;  // color A (sRGB)
    float br, bg, bb;  // color B (sRGB)
    float scale;       // noise cell size in px
    float angle;       // gradient angle, radians
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
    else
    {
        rgb = color_a;
    }
    output[tid.xy] = float4(rgb, 1.0);
}
