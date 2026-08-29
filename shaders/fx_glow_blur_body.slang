// The including file must define GLOW_DIR as 0 for x or 1 for y.

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
    float radius;      // px
    float threshold;
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;

    // CCD smear is a readout column, so the two passes use different radii.
    const bool ccd = int(mode + 0.5) == 2;
    float r = max(radius, 0.5);
    if (ccd)
        r *= (GLOW_DIR == 0) ? 0.05 : 5.0;

    const float2 axis = (GLOW_DIR == 0) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    const float sigma = r * 0.5;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    [unroll]
    for (int i = -8; i <= 8; ++i)
    {
        const float offset = float(i) / 8.0 * r;
        const float w = (ccd && GLOW_DIR == 1)
            ? 1.0 / (1.0 + abs(offset) / max(r, 1.0) * 1.5)
            : exp(-0.5 * (offset * offset) / (sigma * sigma));
        const float2 p = uv + axis * offset / size;
        sum += input.SampleLevel(saturate(p), 0) * w;
        weight_sum += w;
    }
    output[tid.xy] = sum / weight_sum;
}
