// Contour / topo lines (spec §6.6): iso-lines where luma crosses N evenly
// spaced levels — the picture becomes an elevation map. Line width from
// the local luma gradient so lines stay uniform on steep and shallow
// slopes alike. Encoded-space luma.
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
    float levels;
    float thickness;
    float mode;   // 0 white-on-black, 1 black-on-white, 2 over image
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float luma_at(float2 uv)
{
    return fx_luma(fx_srgb_oetf(saturate(input.SampleLevel(uv, 0).rgb)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float n = clamp(round(levels), 3.0, 32.0);
    const float l = luma_at(uv);
    // Manual fwidth: gradient magnitude scaled into level space.
    const float2 e = 1.0 / size;
    const float gx = luma_at(uv + float2(e.x, 0)) - luma_at(uv - float2(e.x, 0));
    const float gy = luma_at(uv + float2(0, e.y)) - luma_at(uv - float2(0, e.y));
    const float grad = max(length(float2(gx, gy)) * 0.5 * n, 1.0e-4);

    const float f = frac(l * n);
    const float dist = min(f, 1.0 - f) / grad;   // px-ish to the iso line
    const float line = 1.0 - smoothstep(thickness * 0.5, thickness, dist);

    const uint m = uint(mode + 0.5);
    float3 rgb;
    if (m == 0u) rgb = fx_srgb_eotf(float3(line, line, line));
    else if (m == 1u) rgb = fx_srgb_eotf(float3(1.0, 1.0, 1.0) * (1.0 - line));
    else rgb = base.rgb * (1.0 - line * 0.8) + line * 0.02;

    const float4 result = float4(rgb, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
