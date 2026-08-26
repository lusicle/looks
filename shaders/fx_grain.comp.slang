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
    float amount;
    float size;        // cell size, px
    float color;       // 0 mono, 1 fully per-channel
    float mode;        // 0 film grain, 1 CCD sensor noise
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

    // CCD noise is per-pixel; film grain is smooth CLUMPS, not square
    // cells: two value-noise octaves per dye layer, the blue layer
    // coarsest and strongest, all re-rolled every frame.
    const bool ccd = mode >= 0.5;
    float3 grain;
    if (ccd) {
        const uint h_mono = fx_hash4(seed, frame, tid.x, tid.y);
        const float mono = fx_hash01(h_mono) - 0.5;
        grain = float3(mono, mono, mono);
        const float3 chroma = float3(
            fx_hash01(fx_hash(h_mono ^ 0x1234567u)) - 0.5,
            fx_hash01(fx_hash(h_mono ^ 0x89ABCDEu)) - 0.5,
            fx_hash01(fx_hash(h_mono ^ 0xF0F0F0Fu)) - 0.5);
        grain = lerp(grain, chroma, saturate(color));
    } else {
        const float cell_size = max(size, 1.0);
        const uint fs = fx_hash3(seed, frame, 0x6A1u);
        const float2 pm = float2(tid.xy) / cell_size;
        const float g_mono =
            (fx_vnoise2(fs, pm) * 0.65 +
             fx_vnoise2(fs ^ 0x2Cu, pm * 2.3) * 0.35 - 0.5) * 2.2;
        const float3 layer_scale = float3(1.0, 1.12, 1.4);
        const float3 layer_amp = float3(0.9, 1.0, 1.45);
        float3 clumps;
        [unroll]
        for (int ch = 0; ch < 3; ++ch) {
            const uint cs = fx_hash(fs ^ (0x51u + uint(ch) * 0x9E3u));
            const float2 pc = pm / layer_scale[ch];
            clumps[ch] =
                (fx_vnoise2(cs, pc) * 0.65 +
                 fx_vnoise2(cs ^ 0x77u, pc * 2.3) * 0.35 - 0.5) * 2.2 *
                layer_amp[ch];
        }
        grain = lerp(float3(g_mono, g_mono, g_mono), clumps,
                     saturate(color));
    }

    const float3 encoded = fx_srgb_oetf(saturate(base.rgb));
    const float l = fx_luma(encoded);
    float weight;
    if (ccd) {
        // Sensor noise is strongest in the shadows, plus a per-row
        // readout banding term.
        weight = 0.25 + 0.75 * (1.0 - l);
        const float row =
            fx_hash01(fx_hash3(seed, frame, tid.y)) - 0.5;
        grain += float3(row, row, row) * 0.3;
    } else {
        // Midtone weighted, since grain lives on film density.
        weight = 4.0 * l * (1.0 - l);
    }

    const float3 grained =
        fx_srgb_eotf(saturate(encoded + grain * amount * weight * 0.35));

    const float4 result = float4(grained, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
