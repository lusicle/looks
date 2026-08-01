// Anamorphic: horizontal squeeze (the stretched-oval geometry
// of a scope lens shown 16:9) plus the signature blue horizontal streak
// flare gathered from thresholded highlights. Linear light throughout.
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
    float streak;      // flare gain
    float threshold;   // highlight gate (encoded-ish)
    float squeeze;     // horizontal squeeze factor about center
    float bokeh;       // oval-bokeh approximation
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

    // Squeeze about the center (sample wider than the frame → pillarboxed
    // stretch reads as the scope look on scope-less footage).
    const float sx = (uv.x - 0.5) * squeeze + 0.5;
    const float2 suv = float2(clamp(sx, 0.0, 1.0), uv.y);
    float3 col = input.SampleLevel(suv, 0).rgb;

    // Streak: gather thresholded highlights along a wide horizontal line.
    const float gate = threshold * threshold;   // rough linear-domain gate
    float3 flare = 0.0;
    const int kTaps = 24;
    [unroll]
    for (int i = 0; i < kTaps; ++i) {
        const float f = (float(i) / float(kTaps - 1)) - 0.5;   // -0.5..0.5
        const float dx = f * 0.6;                              // frame-wide
        const float3 s =
            input.SampleLevel(float2(clamp(sx + dx, 0.0, 1.0), uv.y), 0).rgb;
        const float l = fx_luma(s);
        const float over = max(l - gate, 0.0);
        flare += s * over * (1.0 - abs(f) * 2.0) * (1.0 - abs(f) * 2.0);
    }
    flare /= float(kTaps);
    const float3 blue_tint = float3(0.35, 0.55, 1.0);
    col += flare * blue_tint * streak * 6.0;

    // Oval bokeh: highlights bloom into vertical ellipses —
    // the anamorphic signature. 12 taps on a 2:1 tall ellipse.
    if (bokeh > 0.001) {
        float3 oval = 0.0;
        [unroll]
        for (int i = 0; i < 12; ++i) {
            const float a = float(i) * 0.5236;   // 12 around the ellipse
            const float2 o = float2(cos(a) * 0.5, sin(a)) * bokeh * 14.0;
            const float3 s =
                input.SampleLevel(clamp(suv + o / size, 0.0, 1.0), 0).rgb;
            const float l = fx_luma(s);
            oval += s * max(l - gate, 0.0);
        }
        col += oval / 12.0 * bokeh * 2.0;
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
