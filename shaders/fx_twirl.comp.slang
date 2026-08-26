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
    float angle;      // degrees at center, may exceed a full turn
    float radius;
    float center_x;
    float center_y;
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
    const float4 base = input.SampleLevel(uv, 0);

    const float aspect = size.x / size.y;
    const float2 c = float2(center_x, center_y);
    float2 d = (uv - c) * float2(aspect, 1.0);
    const float r = length(d);
    float3 outc = base.rgb;
    float out_a = base.a;
    if (r < radius) {
        const float f = 1.0 - r / max(radius, 1.0e-3);
        const float a = angle * 0.0174533 * f * f;
        const float ca = cos(a), sa = sin(a);
        d = float2(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
        const float2 suv = c + d / float2(aspect, 1.0);
        const float4 s = input.SampleLevel(clamp(suv, 0.0, 1.0), 0);
        outc = s.rgb;
        out_a = s.a;
    }

    const float4 result = float4(outc, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
