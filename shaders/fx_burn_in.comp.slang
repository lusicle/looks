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
    float mode;
    float threshold;
    float bleed;
    float tint;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float4 held = prev.SampleLevel(uv, 0);

    const float lum = fx_luma(base.rgb);
    const float held_lum = fx_luma(held.rgb);
    const bool seeded = frame > 0u;

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
    const float arm = mode < 1.5 ? step(threshold, lum) : 1.0;
    scar = lerp(seeded ? held : base, scar, arm);
    scar = lerp(scar, base, saturate(bleed));

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
