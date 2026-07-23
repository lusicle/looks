// VHS head-switching band: the displaced, noisy tear strip pinned to the
// bottom of every VCR frame — the video head handoff happens below the
// visible area, and consumer decks show it. The band shears sideways,
// wobbles at field rate, and dissolves into luma hash at its bottom edge.
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
    float band_px;    // band height
    float shift_px;   // horizontal displacement
    float noise;      // hash breakup inside the band
    float wobble;     // shear wander rate, hz
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

    const float band_top = size.y - max(band_px, 1.0);
    float4 result = base;
    if (float(tid.y) >= band_top) {
        // 0 at the band's top edge, 1 at the frame bottom.
        const float depth =
            (float(tid.y) - band_top) / max(band_px, 1.0);
        const float t = fps > 0.0 ? float(frame) / fps : 0.0;
        // Whole-band shear wanders; each row adds its own tug, harder
        // near the bottom where the head has fully let go.
        const float sway =
            (fx_value_noise1(seed, t * max(wobble, 0.01)) - 0.5) * 2.0;
        const float row_jerk =
            (fx_hash01(fx_hash4(seed, frame, tid.y, 3u)) - 0.5) * depth;
        const float dx =
            (sway * (0.35 + 0.65 * depth) + row_jerk * 0.8) * shift_px;
        const float2 suv =
            float2(frac(uv.x + dx / size.x), uv.y);
        float3 c = input.SampleLevel(suv, 0).rgb;
        // Luma hash breakup, saturating toward the bottom edge.
        const float n01 =
            fx_hash01(fx_hash4(seed, frame, tid.x, tid.y));
        const float mixamt = saturate(noise * (0.25 + depth * depth));
        c = lerp(c, float3(n01, n01, n01) * 0.8, mixamt);
        // Chroma drops out in the band (heads lose color lock first).
        const float l = fx_luma(c);
        c = lerp(c, float3(l, l, l), depth * 0.6);
        result = float4(c, base.a);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
