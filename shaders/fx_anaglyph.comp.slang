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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float depth;    // base disparity, px
    float pop;
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D right_eye;
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

    const float l = fx_luma(fx_encoded(base));
    const float shift_px = depth * (history_valid != 0u ? 1.0 : lerp(1.0, l, saturate(pop)));
    const float2 shift = float2(shift_px / canvas_w, 0.0);

    const float4 left4 = fx_sample_border(input, uv - shift);
    const float4 right4 = fx_sample_border(right_eye, uv + shift);
    const float3 left = left4.rgb;
    const float3 right = right4.rgb;

    const uint m = uint(mode + 0.5);
    float3 outc;
    if (m == 1u)
        outc = float3(left.r, 0.0, right.b);
    else if (m == 2u)
        outc = float3(right.r, left.g, right.b);
    else
        outc = max(float3(
            dot(left, float3(0.456100, 0.500484, 0.176381)) + dot(right, float3(-0.043470, -0.087939, -0.001555)),
            dot(left, float3(-0.040082, -0.037824, -0.015759)) + dot(right, float3(0.378476, 0.733640, -0.018450)),
            dot(left, float3(-0.015216, -0.020597, -0.005469)) + dot(right, float3(-0.072153, -0.112961, 1.226400))), 0.0);

    const float4 result = float4(outc, max(left4.a, right4.a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
