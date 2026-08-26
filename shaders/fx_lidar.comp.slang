// The canvas at binding 1 is this instance's own previous output, so the
// point cloud accumulates under the one-frame-delay rule.

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
    float mode;      // 0 scatter, 1 spinning beam, 2 raster sweep
    float amount;
    float dot_px;
    float persist;
    float rate;      // hz
    float bias;
    float spin;      // revolutions or crossings per second
    float color;     // 0 white points, 1 source-colored
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

static const float kTau = 6.28318530718;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    float3 col = canvas.SampleLevel(uv, 0).rgb * saturate(persist);

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = uint(t * max(rate, 0.01));
    const float r_dot = max(dot_px, 0.75);
    // One candidate dot per cell, sized so dots can breathe. The 3x3
    // neighbourhood below is what lets a dot overlap into this pixel.
    const float cell = max(r_dot * 3.0, 6.0);
    const uint m = uint(mode + 0.5);
    const float2 center = size * 0.5;

    const int2 my_cell = int2(float2(tid.xy) / cell);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        const int2 c = my_cell + int2(dx, dy);
        if (c.x < 0 || c.y < 0) continue;
        const uint h = fx_hash4(seed ^ (tick + 1u) * 0x51EDu, tick,
                                uint(c.x), uint(c.y));
        if (fx_hash01(h) >= amount) continue;

        const float2 cpos =
            (float2(c) + float2(fx_hash01(fx_hash(h ^ 0x11u)),
                                fx_hash01(fx_hash(h ^ 0x22u)))) * cell;
        if (cpos.x >= size.x || cpos.y >= size.y) continue;

        if (m == 1u) {
            const float beam = frac(t * spin) * kTau;
            const float2 d = cpos - center;
            float da = abs(atan2(d.y, d.x) - (beam - 3.14159265));
            if (da > 3.14159265) da = kTau - da;
            if (da > 0.13) continue;
        } else if (m == 2u) {
            const float beam_x = frac(t * spin) * size.x;
            if (abs(cpos.x - beam_x) > cell * 2.0) continue;
        }

        const float2 suv = (cpos + 0.5) / size;
        const float3 src = input.SampleLevel(saturate(suv), 0).rgb;
        const float l = fx_luma(fx_srgb_oetf(saturate(src)));
        if (fx_hash01(fx_hash(h ^ 0x33u)) >
            lerp(1.0, saturate(l * 1.4 + 0.05), saturate(bias)))
            continue;

        const float d_px = length(float2(tid.xy) - cpos);
        if (d_px < r_dot) {
            const float fall = 1.0 - d_px / r_dot;
            const float3 dot_col =
                lerp(float3(1.0, 1.0, 1.0), src * 1.5, saturate(color)) *
                (0.35 + 0.65 * l);
            // Max blend keeps points crisp instead of blooming additively.
            col = max(col, dot_col * fall * fall * 1.4);
        }
    }

    const float4 result = float4(min(col, float3(1.6, 1.6, 1.6)),
                                 max(base.a, saturate(fx_luma(col))));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
