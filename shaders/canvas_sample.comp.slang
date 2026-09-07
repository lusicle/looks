#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint row;
    uint clips;
    uint map_height;
    float opacity;
};

[[vk::binding(0, 0)]] Sampler2D source;
[[vk::binding(1, 0)]] Sampler2D maps;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

float4 read_map(uint x, uint y)
{
    return maps.SampleLevel(float2((float(x) + 0.5) / 3.0,
        (float(y) + 0.5) / float(map_height)), 0);
}

float2 mapped(float2 uv, uint r)
{
    return float2(dot(read_map(0, r).xyz, float3(uv, 1.0)),
                  dot(read_map(1, r).xyz, float3(uv, 1.0)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float2 suv = mapped(uv, row);
    float coverage = opacity;
    for (uint i = 1; i <= clips; ++i) {
        const uint r = row + i;
        const float2 cuv = mapped(uv, r);
        const float4 rect = read_map(2, r);
        const float2 pixel = float2(width, height);
        const float2 footprint = float2(length(read_map(0, r).xy / pixel),
                                         length(read_map(1, r).xy / pixel));
        const float2 edge = min(cuv - rect.xy, rect.zw - cuv) / max(footprint, 1e-12);
        coverage = min(coverage, saturate(min(edge.x, edge.y) + 0.5) * opacity);
    }
    output[tid.xy] = source.SampleLevel(clamp(suv, 0.0, 1.0), 0) * coverage;
}
