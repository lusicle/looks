// Separable gaussian feather for extracted masks (spec §8 post). Two
// dispatches (dir 0 = horizontal, 1 = vertical); 13 taps spread over the
// radius — smooth enough for feathering, fully deterministic.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float radius_px;
    uint dir;
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

    if (radius_px < 0.5)
    {
        output[tid.xy] = input.SampleLevel(uv, 0);
        return;
    }

    const float2 axis = dir == 0 ? float2(1.0, 0.0) : float2(0.0, 1.0);
    const float sigma = radius_px * 0.5;
    float sum = 0.0;
    float weight_sum = 0.0;
    [unroll]
    for (int i = -6; i <= 6; ++i)
    {
        const float offset = float(i) / 6.0 * radius_px;
        const float w = exp(-0.5 * (offset * offset) / (sigma * sigma));
        const float2 p = uv + axis * offset / size;
        sum += input.SampleLevel(p, 0).r * w;
        weight_sum += w;
    }
    const float v = sum / weight_sum;
    output[tid.xy] = float4(v, v, v, 1.0);
}
