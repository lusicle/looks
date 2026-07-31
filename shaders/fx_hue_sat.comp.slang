// Hue/Sat (docs/flow_canvas.md v5.5 primitives): hue rotation about the
// gray axis (Rodrigues), saturation toward/away from luma, lightness
// lift. Display-referred core.
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
    float hue;    // degrees, -180..180
    float sat;    // 0 = gray, 1 = unchanged, 2 = doubled
    float lite;   // -1..1 lightness add
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    float3 c = fx_srgb_oetf(saturate(base.rgb));

    // Hue rotation: Rodrigues about the normalized gray axis.
    const float a = hue * 0.01745329252;
    const float ca = cos(a), sa = sin(a);
    const float3 k = float3(0.57735, 0.57735, 0.57735);
    c = c * ca + cross(k, c) * sa + k * dot(k, c) * (1.0 - ca);

    const float l = fx_luma(c);
    c = lerp(float3(l, l, l), c, clamp(sat, 0.0, 2.0));
    c += lite;

    const float4 result = float4(fx_srgb_eotf(saturate(c)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
