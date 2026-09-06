#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push {
    uint source_w;
    uint source_h;
    uint grid_w;
    uint grid_h;
    uint first;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

groupshared float4 partial[64];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID,
          uint lane : SV_GroupIndex)
{
    float4 value = float4(0.0, 0.0, 1.0e30, -1.0e30);
    if (tid.x < grid_w && tid.y < grid_h) {
        float2 offset = 0.5;
        if (first != 0u && (source_w > grid_w || source_h > grid_h))
            offset = float2(fx_hash01(fx_hash3(0x51A7u, tid.x, tid.y)),
                            fx_hash01(fx_hash3(0x93C5u, tid.x, tid.y)));
        uint2 p = min(uint2((float2(tid.xy) + offset) *
            float2(source_w, source_h) / float2(grid_w, grid_h)),
            uint2(source_w - 1u, source_h - 1u));
        float4 s = input.Load(int3(p, 0));
        if (first == 0u) {
            value = s;
        } else if (s.a > 0.0) {
            float3 rgb = s.rgb / s.a;
            value = float4(fx_luma(s.rgb), s.a,
                min(rgb.r, min(rgb.g, rgb.b)), max(rgb.r, max(rgb.g, rgb.b)));
        }
    }
    partial[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            float4 b = partial[lane + stride];
            partial[lane].xy += b.xy;
            partial[lane].z = min(partial[lane].z, b.z);
            partial[lane].w = max(partial[lane].w, b.w);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u) output[gid.xy] = partial[0];
}
