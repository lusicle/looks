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
    float readout;
    float axis;
    float gain;
    float anchor;
    float wobble;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D flow;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float line_t = axis < 0.5 ? uv.y : uv.x;
    float t = (line_t - saturate(anchor)) * readout;

    if (wobble > 0.0)
        t += sin(line_t * 6.2831853) * wobble * readout * 0.35;

    const float2 probe = axis < 0.5 ? float2(uv.x, saturate(anchor))
                                    : float2(saturate(anchor), uv.y);
    const float2 mv = flow.SampleLevel(probe, 0).rg * gain;

    const float2 suv = clamp(uv + mv * t / size, 0.0, 1.0);
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
