// The inks multiply as transmittance.

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
    float inks;      // 1 to 3
    float hue1;
    float hue2;
    float misreg;    // px
    float grain;
    float paper;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float3 riso_color(Sampler2D tex, float2 uv, float2 size, uint seed_v,
                uint ink, float misreg_px)
{
    const uint h = fx_hash3(seed_v, ink + 7u, 0x1517u);
    const float ang = fx_hash01(h) * 6.28318530718;
    const float2 offs =
        float2(cos(ang), sin(ang)) * misreg_px / size;
    return saturate(fx_straight(tex.SampleLevel(clamp(uv + offs, 0.0, 1.0), 0)));
}

float3 separate_inks(float3 rgb, float3 d0, float3 d1, float3 d2, uint count)
{
    float3 target = -log(max(rgb, 1e-5));
    float3 density[3] = {d0, d1, d2};
    float3 coverage = 0.0;
    float3 residual = target;
    for (int pass = 0; pass < 6; ++pass) {
        for (uint i = 0; i < count; ++i) {
            residual += density[i] * coverage[i];
            coverage[i] = saturate(dot(residual, density[i]) / max(dot(density[i], density[i]), 0.01));
            residual -= density[i] * coverage[i];
        }
    }
    return coverage;
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

    const uint n_inks = uint(clamp(inks, 1.0, 3.0) + 0.5);

    // The grain is static per seed. Do not add a time term.
    const uint2 material_px = uint2(uv * float2(canvas_w, canvas_h));
    const float gn =
        (fx_hash01(fx_hash4(seed ^ 0x515Fu, material_px.x, material_px.y, 3u)) - 0.5) *
            min(1.0, float(width) / canvas_w);

    const float pw = saturate(paper);
    const float3 paper_color = fx_srgb_eotf(float3(1.0, 1.0 - 0.05 * pw, 1.0 - 0.14 * pw));
    const float3 d0 = -log(fx_srgb_eotf(fx_hue_to_rgb(hue1) * 0.75 + 0.05));
    const float3 d1 = -log(fx_srgb_eotf(fx_hue_to_rgb(hue2) * 0.8 + 0.1));
    const float3 d2 = -log(fx_srgb_eotf(fx_hue_to_rgb(0.145) * 0.9 + 0.1));
    const float3 densities[3] = {d0, d1, d2};
    float3 density = 0.0;
    for (uint i = 0; i < n_inks; ++i) {
        const float3 src = riso_color(input, uv, float2(canvas_w, canvas_h), seed, i, misreg);
        const float3 coverage = separate_inks(src, d0, d1, d2, n_inks);
        const float c = saturate(coverage[i] + gn * grain * 0.5);
        density += densities[i] * c;
    }

    const float4 result = float4(paper_color * exp(-density) * base.a, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
