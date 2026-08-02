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

    // CCD noise is per-pixel; film grain clumps into ISO-sized cells.
    const bool ccd = mode >= 0.5;
    const float cell_size = ccd ? 1.0 : max(size, 1.0);
    const uint2 cell = uint2(float2(tid.xy) / cell_size);

    const uint h_mono = fx_hash4(seed, frame, cell.x, cell.y);
    const float mono = fx_hash01(h_mono) - 0.5;
    float3 grain = float3(mono, mono, mono);
    const float3 chroma = float3(
        fx_hash01(fx_hash(h_mono ^ 0x1234567u)) - 0.5,
        fx_hash01(fx_hash(h_mono ^ 0x89ABCDEu)) - 0.5,
        fx_hash01(fx_hash(h_mono ^ 0xF0F0F0Fu)) - 0.5);
    grain = lerp(grain, chroma, saturate(color));

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
