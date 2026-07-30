// Watercolor: transparent washes. An edge-stopped disc blur melts detail
// into washes without crossing hard boundaries; pigment pools (darkens)
// along the wash edges; granulation settles coarse pigment into the
// paper tooth in the darker passages; paper white lifts the highlights.
// Display-referred, self-converting.
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
    float bleed;         // wash radius
    float pool;          // edge darkening
    float granulation;   // pigment texture in the darks
    float paper;         // paper tooth + white lift
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float2 kTaps[12] = {
    float2(1.0, 0.0),   float2(0.5, 0.87),   float2(-0.5, 0.87),
    float2(-1.0, 0.0),  float2(-0.5, -0.87), float2(0.5, -0.87),
    float2(0.55, 0.3),  float2(0.0, 0.62),   float2(-0.55, 0.3),
    float2(-0.55, -0.3), float2(0.0, -0.62), float2(0.55, -0.3),
};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float3 center = fx_srgb_oetf(saturate(base.rgb));
    const float radius_px = 2.0 + saturate(bleed) * 9.0;

    // Edge-stopped wash: taps that differ too much in color barely count.
    float3 acc = center;
    float wsum = 1.0;
    for (uint i = 0; i < 12; ++i) {
        const float2 o = kTaps[i] * radius_px / size;
        const float3 s = fx_srgb_oetf(saturate(
            input.SampleLevel(clamp(uv + o, 0.0, 1.0), 0).rgb));
        const float cd = dot(abs(s - center), float3(1.0, 1.0, 1.0));
        const float wgt = exp(-cd * cd * 5.0);
        acc += s * wgt;
        wsum += wgt;
    }
    float3 wash = acc / wsum;

    // Edge pooling: sobel-ish gradient of the input luma darkens the
    // boundaries where pigment collects.
    const float2 px = 1.5 / size;
    const float lr = fx_luma(input.SampleLevel(
        clamp(uv + float2(px.x, 0.0), 0.0, 1.0), 0).rgb);
    const float ll = fx_luma(input.SampleLevel(
        clamp(uv - float2(px.x, 0.0), 0.0, 1.0), 0).rgb);
    const float lu = fx_luma(input.SampleLevel(
        clamp(uv - float2(0.0, px.y), 0.0, 1.0), 0).rgb);
    const float ld = fx_luma(input.SampleLevel(
        clamp(uv + float2(0.0, px.y), 0.0, 1.0), 0).rgb);
    const float gmag = length(float2(lr - ll, ld - lu));
    wash *= 1.0 - saturate(pool) * saturate(gmag * 5.0) * 0.45;

    // Granulation: coarse seeded pigment clumps, strongest in the darks.
    const float l = fx_luma(wash);
    const uint2 cellpix = uint2(tid.xy) / 2u;
    const float gn = fx_hash01(fx_hash4(seed ^ 0x6A09u, cellpix.x,
                                        cellpix.y, 5u));
    wash *= 1.0 - saturate(granulation) * 0.3 * gn * (1.0 - l);

    // Paper: tooth modulation + white lift toward warm paper.
    const float tooth =
        fx_value_noise1(seed ^ 0x33u, float(tid.x) * 0.13) *
        fx_value_noise1(seed ^ 0x44u, float(tid.y) * 0.13);
    wash *= 1.0 + saturate(paper) * 0.18 * (tooth * 2.0 - 0.5);
    wash = lerp(wash, float3(1.0, 0.99, 0.95),
                saturate(paper) * 0.3 * smoothstep(0.55, 1.0, l));

    const float4 result = float4(fx_srgb_eotf(saturate(wash)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
