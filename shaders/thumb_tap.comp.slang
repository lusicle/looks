// Node-canvas thumbnail tap (docs/flow_canvas.md): downsample one graph
// node's output into its 160x90 cell of the thumbnail atlas. The atlas is
// RGBA8 holding sRGB-encoded bytes — the UI image pipeline passes them
// through unchanged, so the OETF happens here, once.

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
    const float2 uv = (float2(tid.xy) + 0.5) / float2(160.0, 90.0);
    const float3 rgb = input.SampleLevel(uv, 0).rgb;
    atlas[uint2(cell_x * 160 + tid.x, cell_y * 90 + tid.y)] =
        float4(fx_srgb_oetf(saturate(rgb)), 1.0);
}
