// Risograph: the image separated into flat ink layers laid over warm
// paper, each layer misregistered along its own seeded direction, grain
// breaking up the flat tints. Ink 1 carries the shadows/detail, ink 2
// floods the midtones, an optional third ink (yellow) lifts highlights.
// Inks multiply (transmittance), like real stencil printing.
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
    float inks;      // ink layer count, 1-3
    float hue1;      // shadow/detail ink
    float hue2;      // midtone flood ink
    float misreg;    // per-ink misregistration, px
    float grain;     // flat-tint break-up
    float paper;     // paper warmth
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Encoded luma at a misregistered tap for one ink layer.
float riso_luma(Sampler2D tex, float2 uv, float2 size, uint seed_v,
                uint ink, float misreg_px)
{
    const uint h = fx_hash3(seed_v, ink + 7u, 0x1517u);
    const float ang = fx_hash01(h) * 6.28318530718;
    const float2 offs =
        float2(cos(ang), sin(ang)) * misreg_px / size;
    const float3 rgb =
        tex.SampleLevel(clamp(uv + offs, 0.0, 1.0), 0).rgb;
    return fx_luma(fx_srgb_oetf(saturate(rgb)));
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

    // Grain: per-pixel hash breaking up flat coverage (static per seed —
    // riso grain is in the print, not the video).
    const float gn =
        fx_hash01(fx_hash4(seed ^ 0x515Fu, tid.x, tid.y, 3u)) - 0.5;

    // Paper: warm white.
    const float pw = saturate(paper);
    float3 enc = float3(1.0, 1.0 - 0.05 * pw, 1.0 - 0.14 * pw);

    // Ink 1: shadows + detail.
    {
        const float l = riso_luma(input, uv, size, seed, 0u, misreg);
        float c = smoothstep(0.9, 0.12, l);
        c = saturate(c + gn * grain * 0.7);
        const float3 ink = fx_hue_to_rgb(hue1) * 0.75 + 0.05;
        enc *= lerp(float3(1.0, 1.0, 1.0), ink, c);
    }
    // Ink 2: midtone flood, flatter curve.
    if (n_inks >= 2u) {
        const float l = riso_luma(input, uv, size, seed, 1u, misreg);
        float c = smoothstep(0.95, 0.3, l) * 0.85;
        c = saturate(c + gn * grain * 0.6);
        const float3 ink = fx_hue_to_rgb(hue2) * 0.8 + 0.1;
        enc *= lerp(float3(1.0, 1.0, 1.0), ink, c);
    }
    // Ink 3: highlight lift, always yellow (the classic third drum).
    if (n_inks >= 3u) {
        const float l = riso_luma(input, uv, size, seed, 2u, misreg);
        float c = smoothstep(0.25, 0.85, l) * 0.6;
        c = saturate(c + gn * grain * 0.5);
        const float3 ink = fx_hue_to_rgb(0.145) * 0.9 + 0.1;
        enc *= lerp(float3(1.0, 1.0, 1.0), ink, c);
    }

    const float4 result = float4(fx_srgb_eotf(saturate(enc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
