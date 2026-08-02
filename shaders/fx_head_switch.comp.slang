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
    float band_px;
    float shift_px;
    float noise;
    float wobble;     // hz
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
        // 0 at the band top edge, 1 at the frame bottom.
        const float depth =
            (float(tid.y) - band_top) / max(band_px, 1.0);
        const float t = fps > 0.0 ? float(frame) / fps : 0.0;
        // Row tug scales with depth, hardest where the head has let go.
        const float sway =
            (fx_value_noise1(seed, t * max(wobble, 0.01)) - 0.5) * 2.0;
        const float row_jerk =
            (fx_hash01(fx_hash4(seed, frame, tid.y, 3u)) - 0.5) * depth;
        const float dx =
            (sway * (0.35 + 0.65 * depth) + row_jerk * 0.8) * shift_px;
        const float2 suv =
            float2(frac(uv.x + dx / size.x), uv.y);
        float3 c = input.SampleLevel(suv, 0).rgb;
        const float n01 =
            fx_hash01(fx_hash4(seed, frame, tid.x, tid.y));
        const float mixamt = saturate(noise * (0.25 + depth * depth));
        c = lerp(c, float3(n01, n01, n01) * 0.8, mixamt);
        // Chroma goes first: heads lose color lock before luma.
        const float l = fx_luma(c);
        c = lerp(c, float3(l, l, l), depth * 0.6);
        result = float4(c, base.a);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
