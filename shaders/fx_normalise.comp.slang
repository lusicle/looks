#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push {
    uint width;
    uint height;
    float wet;
    float opacity;
    uint seed;
    uint frame;
    float fps;
    float quality;
    float midpoint;
    float range;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D measured;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float4 base = input.Load(int3(tid.xy, 0));
    float4 stats = measured.Load(int3(0, 0, 0));
    if (base.a <= 0.0 || stats.y <= 0.0) {
        output[tid.xy] = base;
        return;
    }
    float mean = stats.x / stats.y;
    float target = saturate(midpoint);
    float scale = 1.0;
    if (range < 0.5) {
        if (mean > stats.z) scale = min(scale, target / (mean - stats.z));
        if (stats.w > mean) scale = min(scale, (1.0 - target) / (stats.w - mean));
    }
    float3 rgb = saturate((base.rgb / base.a - mean) * scale + target);
    float4 result = float4(rgb * base.a, base.a);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
