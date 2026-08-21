// Taps stay in linear light so highlights bloom through the streak the
// way real motion blur does.

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
    float amount;   // px, or degrees for radial
    float mode;     // 0 directional, 1 radial, 2 zoom, 3 gauss, 4 tilt,
                    // 5 bokeh, 6 surface
    float angle;
    float focus;    // tilt band center; doubles as surface range weight
    float band;
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
    // Taps accumulate float4: coverage blurs with color, so a
    // transparent layer's edge feathers instead of smearing black.
    float4 blurred;
    if (m >= 3u) {
        // Disc-tap family: 24 golden-spiral taps, per-mode weights.
        float radius = amount * 0.5;
        if (m == 4u) {
            const float2 axis = float2(-sin(angle), cos(angle));
            const float pos = dot(uv - 0.5, axis) + 0.5;
            const float d = abs(pos - focus);
            radius *= saturate((d - band) / max(band, 1.0e-3));
        }
        float4 acc = base;
        float wsum = 1.0;
        [unroll]
        for (int i = 0; i < 24; ++i) {
            const float fi = (float(i) + 0.5) / 24.0;
            const float a = float(i) * 2.39996;   // golden angle, radians
            const float r = sqrt(fi) * radius;
            const float2 suv = uv + float2(cos(a), sin(a)) * r / size;
            const float4 c = input.SampleLevel(clamp(suv, 0.0, 1.0), 0);
            float wgt;
            if (m == 5u) {
                // Energy-weighted so bright points bloom into discs.
                const float lum = fx_luma(c.rgb);
                wgt = 1.0 + lum * lum * 6.0;
            } else if (m == 6u) {
                // Bilateral: spatial falloff x range weight, so texture
                // smooths while real edges hold.
                const float3 dc = c.rgb - base.rgb;
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
    float4 acc = 0.0;
    [unroll]
    for (int i = 0; i < kTaps; ++i) {
        const float f = (float(i) / float(kTaps - 1)) - 0.5;
        float2 suv;
        if (m == 0u) {
            const float2 dir = float2(cos(angle), sin(angle));
            suv = uv + dir * f * amount / size;
        } else if (m == 1u) {
            const float a = f * amount * 0.0174533;
            const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
            const float ca = cos(a), sa = sin(a);
            const float2 rd = float2(d.x * ca - d.y * sa,
                                     d.x * sa + d.y * ca);
            suv = rd / float2(size.x / size.y, 1.0) + 0.5;
        } else {
            suv = uv + (uv - 0.5) * f * amount * 2.0 / size.y;
        }
        acc += input.SampleLevel(clamp(suv, 0.0, 1.0), 0);
    }
    blurred = acc / float(kTaps);
    }
    const float4 wetted = lerp(base, blurred, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
