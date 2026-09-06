// Keep the tap count at 9 for all radius values.

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
    float radius;   // px
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 px = 1.0 / float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) * px;
    const float4 base = input.SampleLevel(uv, 0);

    // Bilinear taps scale the footprint with radius. Do not add taps.
    const float2 r = px * max(radius, 0.01);
    float4 blur = base * 0.25;
    blur += input.SampleLevel(uv + float2(r.x, 0.0), 0) * 0.125;
    blur += input.SampleLevel(uv - float2(r.x, 0.0), 0) * 0.125;
    blur += input.SampleLevel(uv + float2(0.0, r.y), 0) * 0.125;
    blur += input.SampleLevel(uv - float2(0.0, r.y), 0) * 0.125;
    blur += input.SampleLevel(uv + r, 0) * 0.0625;
    blur += input.SampleLevel(uv - r, 0) * 0.0625;
    blur += input.SampleLevel(uv + float2(r.x, -r.y), 0) * 0.0625;
    blur += input.SampleLevel(uv + float2(-r.x, r.y), 0) * 0.0625;

    const float3 sharpened =
        max(base.rgb + (base.rgb - fx_straight(blur) * base.a) * amount, 0.0);

    const float4 wetted =
        lerp(base, float4(sharpened, base.a), saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
