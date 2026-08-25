// Downsamples one graph node into its 160x90 atlas cell. The atlas
// holds LINEAR working-space values like every texture the UI samples;
// the sRGB swapchain applies the transfer exactly once at composite,
// so card thumbs match the monitor by construction.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint cell_x;
    uint cell_y;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> atlas;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= 160 || tid.y >= 90)
        return;
    // One bilinear tap undersamples big sources (a 4K frame gives each
    // thumb pixel a 24x24 footprint) and aliases dither into shimmer:
    // stratified 6x6 box average instead, still linear-space.
    const float2 cell = 1.0 / float2(160.0, 90.0);
    const float2 uv0 = float2(tid.xy) * cell;
    float3 acc = float3(0.0, 0.0, 0.0);
    for (int j = 0; j < 6; ++j)
        for (int i = 0; i < 6; ++i) {
            const float2 o = (float2(i, j) + 0.5) / 6.0;
            acc += input.SampleLevel(uv0 + o * cell, 0).rgb;
        }
    atlas[uint2(cell_x * 160 + tid.x, cell_y * 90 + tid.y)] =
        float4(saturate(acc / 36.0), 1.0);
}
