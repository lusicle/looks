// Direct flash: on-camera flash look — a hard center-weighted
// lift with steep radial falloff, flattened shadows, and the cold white
// cast of a xenon tube. Linear light (it IS light).
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
    float strength;
    float falloff;   // radial exponent — higher = harder edge
    float cool;      // blue-white tint amount
    float red_eye;   // red lift in dark center regions
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

    const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
    const float r = length(d) * 1.35;
    const float lit = exp(-pow(max(r, 0.0), 2.0) * falloff);

    const float3 flash_tint =
        lerp(float3(1.0, 1.0, 1.0), float3(0.85, 0.92, 1.15), saturate(cool));
    // Multiply-boost what the flash reaches, lift its floor slightly
    // (bounce fill), leave the falloff to drop into underexposure.
    float3 col = base.rgb * (1.0 + lit * strength * 2.0) *
                 lerp(float3(1.0, 1.0, 1.0), flash_tint, lit);
    col += lit * strength * 0.02;              // flat fill
    col *= lerp(0.55, 1.0, lit);               // background falls off

    // Red-eye: the retina bounce — dark pupil-like regions
    // near the center pick up a red glow under the flash.
    if (red_eye > 0.001) {
        const float l = fx_luma(base.rgb);
        const float dark = saturate(1.0 - l * 6.0);
        col.r += dark * dark * lit * red_eye * 0.9;
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
