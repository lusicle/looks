// Sum the flare energy in linear light. Encoded sums make dull streaks.

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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float streak;
    float threshold;
    float squeeze;
    float bokeh;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D filtered;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float sx = (uv.x - 0.5) * squeeze + 0.5;
    const float2 suv = float2(clamp(sx, 0.0, 1.0), uv.y);
    const float4 squeezed = fx_sample_border(input, float2(sx, uv.y));
    float3 col = squeezed.rgb;
    float col_a = squeezed.a;

    const float gate = threshold * threshold;
    float3 flare = 0.0;
    const int kTaps = 24;
    [unroll]
    for (int i = 0; i < kTaps; ++i) {
        const float f = (float(i) / float(kTaps - 1)) - 0.5;
        const float dx = f * 0.6;
        const float3 s =
            fx_sample_border(filtered, float2(sx + dx, uv.y)).rgb;
        flare += s * (1.0 - abs(f) * 2.0) * (1.0 - abs(f) * 2.0);
    }
    flare /= float(kTaps);
    const float3 blue_tint = float3(0.35, 0.55, 1.0);
    col += flare * blue_tint * streak * 6.0;
    col_a = max(col_a, saturate(fx_luma(flare) * streak * 6.0));

    if (bokeh > 0.001) {
        float4 oval = 0.0;
        [unroll]
        for (int i = 0; i < 32; ++i) {
            const float a = float(i) * 2.3999632;
            const float2 o = float2(cos(a) * 0.5, sin(a)) * bokeh * 14.0 * sqrt((float(i) + 0.5) / 32.0);
            const float4 s =
                fx_sample_border(input, suv + o / float2(canvas_w, canvas_h));
            oval += s;
        }
        col += (oval.rgb / 32.0 - squeezed.rgb) * saturate(bokeh);
        col_a = max(lerp(squeezed.a, oval.a / 32.0, saturate(bokeh)),
                    saturate(fx_luma(flare) * streak * 6.0));
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
