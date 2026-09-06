#include "fx_common.slang"
[[vk::push_constant]]
cbuffer Push { uint width; uint height; float threshold; float amount; float radius; };
[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;
[[vk::binding(2, 0)]] RWTexture2D<float4> columns;
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y != 0) return;
    float4 sum = 0.0;
    float3 carry = 0.0;
    float retain = radius > 0.0 ? exp(-1.0 / radius) : 0.0;
    for (uint y = 0; y < height; ++y) {
        float4 s = input.SampleLevel((float2(tid.x, y) + 0.5) / float2(width, height), 0);
        sum += s;
        float3 charge = max(s.rgb, 0.0) + max(s.rgb - threshold, 0.0) * amount;
        float3 well = min(charge, 1.0);
        float3 deposit = min(carry, 1.0 - well);
        carry = (carry - deposit + max(charge - 1.0, 0.0) * 0.5) * retain;
        float3 rgb = well + deposit;
        output[uint2(tid.x, y)] = float4(rgb, max(s.a, max(rgb.r, max(rgb.g, rgb.b))));
    }
    carry = 0.0;
    for (uint row = height; row > 0u; --row) {
        uint y = row - 1u;
        float4 s = input.SampleLevel((float2(tid.x, y) + 0.5) / float2(width, height), 0);
        float3 charge = max(s.rgb, 0.0) + max(s.rgb - threshold, 0.0) * amount;
        float4 current = output[uint2(tid.x, y)];
        float3 deposit = min(carry, max(1.0 - current.rgb, 0.0));
        carry = (carry - deposit + max(charge - 1.0, 0.0) * 0.5) * retain;
        current.rgb += deposit;
        current.a = max(current.a, max(current.r, max(current.g, current.b)));
        output[uint2(tid.x, y)] = current;
    }
    columns[uint2(tid.x, 0)] = sum / float(height);
}
