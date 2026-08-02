// Each frame contributes only a jittered subset of taps, so the trace
// accumulates over frames on the decaying canvas at binding 1, which is
// this instance's own previous output.

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
    float mode;      // 0 waveform, 1 rgb parade, 2 vectorscope
    float gain;
    float glow;
    float persist;
    float graticule;
    float color;     // 0 green phosphor, 1 signal color
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

static const float3 kPhosphor = float3(0.25, 1.0, 0.45);

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
    const uint tick = uint(t * 24.0);
    const uint m = uint(mode + 0.5);
    const float g = saturate(glow);

    if (m <= 1u) {
        // Sample down this column, light up where the level crosses this
        // row. Level space is encoded, matching a real meter.
        const uint third = min(uint(uv.x * 3.0), 2u);
        const float u = m == 1u ? frac(uv.x * 3.0) : uv.x;
        const float level = 1.0 - uv.y;
        float acc = 0.0;
        float3 sig = float3(0.0, 0.0, 0.0);
        for (uint k = 0; k < 48; ++k) {
            const float j =
                fx_hash01(fx_hash3(seed ^ tick * 0x9E3Bu, k, 0x55u));
            const float v = (float(k) + j) / 48.0;
            const float3 enc = fx_srgb_oetf(saturate(
                input.SampleLevel(float2(u, v), 0).rgb));
            const float val =
                m == 1u ? enc[third] : fx_luma(enc);
            const float d =
                (val * saturate(gain * 0.5 + 0.5) - level) * size.y;
            const float a = exp(-d * d * 0.25);
            acc += a;
            sig += input.SampleLevel(float2(u, v), 0).rgb * a;
        }
        float3 trace_col;
        if (m == 1u) {
            const float3 tint = third == 0u ? float3(1.0, 0.25, 0.2)
                              : third == 1u ? float3(0.25, 1.0, 0.3)
                                            : float3(0.3, 0.4, 1.0);
            trace_col = tint;
        } else {
            trace_col = lerp(kPhosphor,
                             sig / max(acc, 1e-4) * 1.6, saturate(color));
        }
        col = max(col, trace_col * acc * 0.35 * g);

        // Level lines at 0, .25, .5, .75, 1.
        const float lf = frac(level * 4.0);
        const float ld = min(lf, 1.0 - lf) * size.y * 0.25;
        col += kPhosphor * saturate(graticule) * 0.10 *
               exp(-ld * ld * 2.0);
    } else {
        // Samples land at (Cb, -Cr) on a centered square.
        const float2 ndc =
            (float2(tid.xy) - size * 0.5) / (0.5 * size.y);
        for (uint k = 0; k < 64; ++k) {
            const uint hk = fx_hash3(seed ^ tick * 0x2F6Bu, k, 0x33u);
            const float su =
                (float(k % 8u) + fx_hash01(hk)) / 8.0;
            const float sv =
                (float(k / 8u) + fx_hash01(fx_hash(hk))) / 8.0;
            const float3 rgb =
                input.SampleLevel(float2(su, sv), 0).rgb;
            const float3 enc = fx_srgb_oetf(saturate(rgb));
            const float l = fx_luma(enc);
            const float cb = (enc.b - l) * 0.564;
            const float cr = (enc.r - l) * 0.713;
            const float2 pos = float2(cb, -cr) * 1.8 * gain;
            const float d_px = length(pos - ndc) * 0.5 * size.y;
            const float a = exp(-d_px * d_px * 0.28);
            const float3 dot_col =
                lerp(kPhosphor, rgb * 1.6, saturate(color));
            col = max(col, dot_col * a * g * 1.2);
        }
        const float r = length(ndc);
        const float ring =
            exp(-pow((r - 0.9) * size.y * 0.5, 2.0) * 0.5);
        const float cross_d =
            min(abs(ndc.x), abs(ndc.y)) * 0.5 * size.y;
        const float cross =
            exp(-cross_d * cross_d * 2.0) * step(r, 0.9);
        col += kPhosphor * saturate(graticule) * 0.12 * (ring + cross);
    }

    const float4 result =
        float4(min(col, float3(1.5, 1.5, 1.5)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
