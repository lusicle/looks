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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float edge_wob =
        (fx_hash01(fx_hash3(seed ^ 0x6E2u, frame, 5u)) - 0.5) * 3.0;
    const float band_top = size.y - max(band_px, 1.0) + edge_wob;

    float4 result = base;
    if (float(tid.y) >= band_top) {
        const float depth =
            (float(tid.y) - band_top) / max(band_px, 1.0);
        const float sway =
            (fx_value_noise1(seed, t * max(wobble, 0.01)) - 0.5) * 2.0;
        const float dx = sway * (0.55 + 0.45 * depth) * shift_px;
        const float2 suv =
            float2(frac(uv.x + dx / size.x), uv.y);
        const float4 tugged = input.SampleLevel(suv, 0);
        float3 c = tugged.rgb;
        float c_a = tugged.a;

        const float streak = fx_vnoise2(
            seed ^ 0x31Bu,
            float2(float(tid.x) / 7.0,
                   float(tid.y) * 2.7 + float(frame) * 91.0)) - 0.5;
        const float mixamt =
            saturate(noise * (0.2 + depth * depth) * 1.4);
        c = saturate(c + streak * mixamt * 1.1);

        const float l = fx_luma(c);
        c = lerp(c, float3(l, l, l), depth * 0.7);

        const float from_edge = float(tid.y) - band_top;
        if (from_edge < 2.0) {
            const float sp =
                fx_hash01(fx_hash4(seed, frame, tid.x / 3u, 9u));
            if (sp > 0.45) {
                const float lvl = 0.5 + 0.5 * sp;
                c = lerp(c, float3(lvl, lvl, lvl), noise * 0.9);
                c_a = max(c_a, noise * 0.9);
            }
        }
        result = float4(c, c_a);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
