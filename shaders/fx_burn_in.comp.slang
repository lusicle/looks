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
    float mode;
    float threshold;
    float bleed;
    float tint;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;
[[vk::binding(3, 0)]] RWTexture2D<float4> state_out;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float4 held = prev.SampleLevel(uv, 0);
    if (mode >= 2.5) {
        const float3 exposure = max(base.rgb - threshold * base.a, 0.0);
        const float3 dose = (history_valid != 0 ? held.rgb * fx_decay(1.0 - bleed, fps) : 0.0) +
            exposure / (max(fps, 1.0) * 30.0);
        state_out[tid.xy] = float4(min(dose, 16.0), 1.0);
        const float3 wear = exp(-dose * lerp(float3(1.0, 1.0, 1.0), float3(0.8, 1.0, 1.3), saturate(tint)));
        output[tid.xy] = lerp(base, float4(base.rgb * wear, base.a), saturate(wet) * saturate(opacity));
        return;
    }

    const float lum = fx_luma(base.rgb);
    const float held_lum = fx_luma(held.rgb);
    const bool seeded = history_valid != 0;

    float4 scar = base;
    if (seeded)
    {
        if (mode < 0.5)
            scar = lum > held_lum ? base : held;
        else if (mode < 1.5)
            scar = lum < held_lum ? base : held;
        else
            scar = held_lum > 0.0 || held.a > 0.0 ? held : base;
    }
    const float arm = mode < 0.5 ? step(threshold, lum) :
                      mode < 1.5 ? step(lum, 1.0 - threshold) : 1.0;
    scar = lerp(seeded ? held : base, scar, arm);
    scar = lerp(base, scar, fx_decay(1.0 - bleed, fps));
    state_out[tid.xy] = scar;

    float3 shown = scar.rgb;
    if (tint > 0.0)
    {
        const float s = fx_luma(scar.rgb);
        const float3 warm = float3(s * 1.25, s * 0.82, s * 0.55);
        shown = lerp(shown, warm, saturate(tint));
    }

    const float4 result = float4(shown, max(base.a, scar.a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
