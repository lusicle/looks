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
    float amount;   // displacement, px
    float freq;     // cycles across the frame
    float speed;    // hz
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float kTau = 6.28318530718;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float t = (fps > 0.0 ? float(frame) / fps : 0.0) * speed * kTau;
    const uint m = uint(mode + 0.5);
    float2 offset = 0.0;
    if (m == 0u) {
        offset.x = sin(uv.y * freq * kTau + t);
    } else if (m == 1u) {
        offset.y = sin(uv.x * freq * kTau + t);
    } else {
        const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
        const float r = length(d);
        const float2 dir = r > 1.0e-4 ? d / r : float2(0.0, 0.0);
        offset = dir * sin(r * freq * kTau - t) *
                 float2(size.y / size.x, 1.0);
    }
    const float2 suv = clamp(uv + offset * amount / size, 0.0, 1.0);
    const float4 result = input.SampleLevel(suv, 0);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
