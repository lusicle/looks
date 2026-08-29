// The atlas holds linear values. The sRGB swapchain encodes them.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint cell_x;
    uint cell_y;
    uint src_w;
    uint src_h;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> atlas;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= 160 || tid.y >= 90)
        return;
    const float cell_aspect = 160.0 / 90.0;
    const float src_aspect =
        float(max(src_w, 1u)) / float(max(src_h, 1u));
    float2 fit = float2(1.0, 1.0);
    if (src_aspect > cell_aspect)
        fit.y = cell_aspect / src_aspect;
    else
        fit.x = src_aspect / cell_aspect;
    // Use a 6x6 box average. One bilinear tap shimmers.
    const float2 cell = 1.0 / float2(160.0, 90.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    for (int j = 0; j < 6; ++j)
        for (int i = 0; i < 6; ++i) {
            const float2 o = (float2(i, j) + 0.5) / 6.0;
            const float2 cuv =
                (float2(tid.xy) + o) * cell;
            const float2 rel = (cuv - 0.5) / fit;
            if (abs(rel.x) <= 0.5 && abs(rel.y) <= 0.5)
                acc += input.SampleLevel(rel + float2(0.5, 0.5), 0).rgb;
        }
    atlas[uint2(cell_x * 160 + tid.x, cell_y * 90 + tid.y)] =
        float4(saturate(acc / 36.0), 1.0);
}
