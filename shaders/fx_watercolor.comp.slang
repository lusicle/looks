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
    float bleed;
    float pool;
    float granulation;
    float paper;
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

    const float3 center = saturate(fx_straight(base));
    const float radius_px = 2.0 + saturate(bleed) * 9.0;

    float3 acc = pow(1.0 - center, 2.0) / max(2.0 * center, 2e-6) * base.a;
    float acc_a = base.a;
    float wsum = 1.0;
    for (uint i = 0; i < 12; ++i) {
        const float2 o = kTaps[i] * radius_px / float2(canvas_w, canvas_h);
        const float4 raw =
            input.SampleLevel(clamp(uv + o, 0.0, 1.0), 0);
        const float3 s = saturate(fx_straight(raw));
        const float tooth = fx_vnoise2(seed, (uv + o) * float2(canvas_w, canvas_h) / 12.0);
        const float wgt = saturate(bleed) * lerp(1.0, 0.35 + tooth, saturate(paper));
        acc += pow(1.0 - s, 2.0) / max(2.0 * s, 2e-6) * (wgt * raw.a);
        acc_a += raw.a * wgt;
        wsum += wgt;
    }
    const float3 density = acc / max(acc_a, 1e-6);
    float3 wash = 1.0 / (1.0 + density + sqrt(density * density + 2.0 * density));
    const float wash_a = acc_a / wsum;

    const float2 px = 1.5 / float2(canvas_w, canvas_h);
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

    const float l = fx_luma(wash);
    const float2 material_px = uv * float2(canvas_w, canvas_h);
    const uint2 cellpix = uint2(material_px / 2.0);
    const float gn = fx_hash01(fx_hash4(seed ^ 0x6A09u, cellpix.x,
                                        cellpix.y, 5u));
    wash *= 1.0 - saturate(granulation) * 0.3 * gn * (1.0 - l);

    const float tooth =
        fx_value_noise1(seed ^ 0x33u, material_px.x * 0.13) *
        fx_value_noise1(seed ^ 0x44u, material_px.y * 0.13);
    wash *= 1.0 + saturate(paper) * 0.18 * (tooth * 2.0 - 0.5);
    wash = lerp(wash, fx_srgb_eotf(float3(1.0, 0.99, 0.95)),
                saturate(paper) * 0.3 * smoothstep(0.55, 1.0, l));

    const float4 result = float4(saturate(wash) * wash_a, wash_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
