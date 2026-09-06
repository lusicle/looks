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
    const float2 material_px = uv * float2(canvas_w, canvas_h);
    const uint tick = uint(t * 59.94);
    const float edge_wob =
        (fx_value_noise1(seed ^ 0x6E2u, t * wobble) - 0.5) * 3.0;
    const float band_top = canvas_h - max(band_px, 0.0) + edge_wob * step(0.001, band_px);

    float4 result = base;
    if (material_px.y >= band_top && band_px > 0.0) {
        const float depth =
            (material_px.y - band_top) / max(band_px, 1.0);
        const float sway =
            (fx_value_noise1(seed, t * max(wobble, 0.0)) - 0.5) * 2.0;
        const float dx = sway * (0.55 + 0.45 * depth) * shift_px;
        const float2 suv =
            float2(frac(uv.x + dx / canvas_w), uv.y);
        const float4 tugged = input.SampleLevel(suv, 0);
        float3 c = fx_encoded(tugged);
        float c_a = tugged.a;

        const float streak = fx_vnoise2(
            seed ^ 0x31Bu,
            float2(material_px.x / 7.0,
                   material_px.y * 2.7 + float(tick) * 91.0)) - 0.5;
        const float mixamt =
            saturate(noise * (0.2 + depth * depth) * 1.4);
        c = saturate(c + streak * mixamt * 1.1);

        const float l = fx_luma(c);
        c = lerp(c, float3(l, l, l), depth * 0.7);

        const float from_edge = material_px.y - band_top;
        if (from_edge < 2.0) {
            const float sp =
                fx_hash01(fx_hash4(seed, frame, tid.x / 3u, 9u));
            if (sp > 0.45) {
                const float lvl = 0.5 + 0.5 * sp;
                c = lerp(c, float3(lvl, lvl, lvl), noise * 0.9);
                c_a = max(c_a, noise * 0.9);
            }
        }
        result = fx_from_encoded(c, c_a);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
