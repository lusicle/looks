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
    float contrast;
    float generations;
    float toner;
    float streaks;
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
    const float2 material_px = uv * float2(canvas_w, canvas_h);

    float4 blurred = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float weight = float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16.0;
            const float2 offset = float2(x, y) * 0.7 / float2(canvas_w, canvas_h);
            blurred += input.SampleLevel(saturate(uv + offset), 0) * weight;
        }
    float g = fx_luma(fx_encoded(blurred));

    // The roller streaks are static in time. Do not add a time term.
    const float streak_n =
        fx_value_noise1(seed ^ 0x51EDu, material_px.x * 0.02) +
        0.35 * fx_value_noise1(seed ^ 0xA3u, material_px.x * 0.11);
    const float thresh =
        0.55 + (streak_n / 1.35 - 0.5) * 0.4 * saturate(streaks);

    const float w = lerp(0.5, 0.05, saturate(contrast));
    g = smoothstep(thresh - w, thresh + w, g);

    const uint h = fx_hash4(seed, uint(material_px.x), uint(material_px.y), 0u);
    const float sp = fx_hash01(h);
    if (sp < toner * 0.05 * g)
        g = 0.08;
    else if (sp > 1.0 - toner * 0.06 * (1.0 - g))
        g = lerp(g, 0.75, 0.8);

    const float3 reflected = lerp(fx_srgb_eotf(float3(0.05, 0.05, 0.07)),
                                  fx_srgb_eotf(float3(0.97, 0.96, 0.93)), g);

    const float4 result = float4(reflected * base.a, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
