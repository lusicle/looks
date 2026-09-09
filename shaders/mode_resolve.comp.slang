#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push {
    uint width;
    uint height;
    uint count;
    uint stride;
    uint offset;
    float tolerance;
    uint sample_y;
};

[[vk::binding(0, 0)]] Sampler2D samples;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float4 comparison(float4 value)
{
    return value.a > 0.00001 ? float4(value.rgb / value.a, value.a) : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float4 values[128];
    for (uint i = 0; i < count; ++i)
        values[i] = comparison(samples.Load(int3(tid.x, tid.y + sample_y + stride * i, 0)));
    uint best = 0;
    uint support = 0;
    for (uint i = 0; i < count; ++i) {
        uint hits = 0;
        for (uint j = 0; j < count; ++j) {
            float4 delta = abs(values[i] - values[j]);
            hits += max(max(delta.r, delta.g), max(delta.b, delta.a)) <= tolerance ? 1 : 0;
        }
        if (hits > support) {
            best = i;
            support = hits;
        }
        if (support == count) break;
    }
    output[uint2(tid.x, tid.y + offset)] = samples.Load(int3(tid.x, tid.y + sample_y + stride * best, 0));
}
