// Blur family (spec §6.5): directional streak, spin (arc around the
// center), zoom (along the ray from center), gaussian (spiral disc taps),
// tilt-shift (gaussian scaled outside a sharp band), bokeh (flat disc
// aperture — highlights bloom into discs), and surface (bilateral,
// edge-preserving) — taps in linear light so highlights bloom through
// the streak like real motion blur.
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

#include "fx_common.slang"

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
    float amount;   // px (directional/zoom/gauss) or degrees-ish (radial)
    float mode;     // 0 directional, 1 radial, 2 zoom, 3 gauss, 4 tilt
    float angle;    // directional streak / tilt band angle
    float focus;    // tilt-shift: band center (0..1 along the frame)
    float band;     // tilt-shift: sharp half-band width
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const uint m = uint(mode + 0.5);
    float3 blurred;
    if (m >= 3u) {
        // Disc-tap family: gaussian, tilt-shift, bokeh, surface — 24
        // golden-spiral taps, per-mode weights. Tilt (mode 4) scales the
        // radius by the distance outside a sharp band along `angle`.
        float radius = amount * 0.5;
        if (m == 4u) {
            const float2 axis = float2(-sin(angle), cos(angle));
            const float pos = dot(uv - 0.5, axis) + 0.5;
            const float d = abs(pos - focus);
            radius *= saturate((d - band) / max(band, 1.0e-3));
        }
        const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
        float3 acc = base.rgb;
        float wsum = 1.0;
        [unroll]
        for (int i = 0; i < 24; ++i) {
            const float fi = (float(i) + 0.5) / 24.0;
            const float a = float(i) * 2.39996;   // golden angle
            const float r = sqrt(fi) * radius;
            const float2 suv = uv + float2(cos(a), sin(a)) * r / size;
            const float3 c = input.SampleLevel(clamp(suv, 0.0, 1.0), 0).rgb;
            float wgt;
            if (m == 5u) {
                // Bokeh: flat disc (hard aperture edge) + energy-weighted
                // highlights so bright points bloom into visible discs.
                const float lum = dot(c, kLuma);
                wgt = 1.0 + lum * lum * 6.0;
            } else if (m == 6u) {
                // Surface: bilateral — spatial falloff x range weight, so
                // texture smooths while real edges hold.
                const float3 dc = c - base.rgb;
                const float range = max(focus * focus, 0.002);
                wgt = (1.0 - fi * 0.7) * exp(-dot(dc, dc) / range);
            } else {
                wgt = 1.0 - fi * 0.7;
            }
            acc += c * wgt;
            wsum += wgt;
        }
        blurred = acc / wsum;
    } else {
    const int kTaps = 16;
    float3 acc = 0.0;
    [unroll]
    for (int i = 0; i < kTaps; ++i) {
        const float f = (float(i) / float(kTaps - 1)) - 0.5;   // -0.5..0.5
        float2 suv;
        if (m == 0u) {
            const float2 dir = float2(cos(angle), sin(angle));
            suv = uv + dir * f * amount / size;
        } else if (m == 1u) {
            // Rotate around the center by up to ±amount/2 "degrees".
            const float a = f * amount * 0.0174533;
            const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
            const float ca = cos(a), sa = sin(a);
            const float2 rd = float2(d.x * ca - d.y * sa,
                                     d.x * sa + d.y * ca);
            suv = rd / float2(size.x / size.y, 1.0) + 0.5;
        } else {
            suv = uv + (uv - 0.5) * f * amount * 2.0 / size.y;
        }
        acc += input.SampleLevel(clamp(suv, 0.0, 1.0), 0).rgb;
    }
    blurred = acc / float(kTaps);
    }
    const float4 result = float4(blurred, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
