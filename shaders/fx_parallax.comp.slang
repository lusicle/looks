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
    float depth_src;
    float pan;
    float tilt;
    float skew;
    float dolly;
    float strength;
    float gamma;
    float fill;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D aux;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

static const int kSteps = 12;

float depth_at(float2 uv)
{
    const float3 c = depth_src < 0.5 ? input.SampleLevel(uv, 0).rgb
                                     : aux.SampleLevel(uv, 0).rgb;
    return pow(saturate(fx_luma(c)), max(gamma, 0.01));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 c = uv - 0.5;
    float2 shift = float2(pan, tilt);
    shift += float2(skew * c.y, skew * c.x);
    shift += c * dolly;
    shift *= strength;

    float2 hit = uv;
    float last_d = 0.0;
    float last_t = 0.0;
    bool found = false;
    for (int i = 1; i <= kSteps; ++i)
    {
        const float t = float(i) / float(kSteps);
        const float2 suv = clamp(uv + shift * t, 0.0, 1.0);
        const float d = depth_at(suv);
        if (d >= t)
        {
            const float span = max(d - t - (last_d - last_t), 1.0e-4);
            const float f = saturate((last_d - last_t) / span);
            const float tt = lerp(last_t, t, f);
            hit = clamp(uv + shift * tt, 0.0, 1.0);
            found = true;
            break;
        }
        last_d = d;
        last_t = t;
    }
    if (!found)
        hit = clamp(uv + shift, 0.0, 1.0);

    float4 result = input.SampleLevel(hit, 0);
    if (fill > 0.5 && !found)
        result = 0.0;

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
