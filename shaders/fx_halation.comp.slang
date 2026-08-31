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
    float threshold;
    float radius;
    float hue;
    float sat;
    float strength;
    float absorb;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const int kTaps = 32;

float3 tint_of(float h, float s)
{
    const float a = h * 6.2831853;
    const float3 warm =
        saturate(float3(1.0, 0.45 + 0.35 * cos(a), 0.18 + 0.3 * sin(a)));
    return lerp(float3(1.0, 1.0, 1.0), warm, saturate(s));
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

    float scatter = 0.0;
    float wsum = 0.0;
    const float golden = 2.3999632;
    for (int i = 0; i < kTaps; ++i)
    {
        const float t = (float(i) + 0.5) / float(kTaps);
        const float theta = float(i) * golden;
        const float2 d = float2(cos(theta), sin(theta)) * sqrt(t);
        const float2 suv = clamp(uv + d * radius / size, 0.0, 1.0);
        const float lum = fx_luma(input.SampleLevel(suv, 0).rgb);
        const float hot = max(lum - threshold, 0.0) / max(1.0 - threshold,
                                                          1.0e-3);
        const float wgt = 1.0 - sqrt(t) * 0.75;
        scatter += hot * wgt;
        wsum += wgt;
    }
    scatter = wsum > 0.0 ? scatter / wsum : 0.0;

    const float3 halo = tint_of(hue, sat) * scatter * strength;
    const float3 sunk = base.rgb * (1.0 - saturate(absorb) * scatter);
    const float3 shown = sunk + halo * base.a;

    const float4 result = float4(shown, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
