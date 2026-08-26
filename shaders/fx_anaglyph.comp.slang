// Disparity rides luma as a depth proxy, so bright subjects read as near.

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
    float depth;    // base disparity, px
    float pop;
    float mode;     // 0 red/cyan, 1 red/blue, 2 green/magenta
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

    const float l = fx_luma(fx_srgb_oetf(saturate(base.rgb)));
    const float shift_px = depth * lerp(1.0, l, saturate(pop));
    const float2 shift = float2(shift_px / size.x, 0.0);

    const float4 left4 = input.SampleLevel(clamp(uv - shift, 0.0, 1.0), 0);
    const float4 right4 = input.SampleLevel(clamp(uv + shift, 0.0, 1.0), 0);
    const float3 left = left4.rgb;
    const float3 right = right4.rgb;

    const uint m = uint(mode + 0.5);
    float3 outc;
    if (m == 1u)         // red/blue: green channel mostly dies
        outc = float3(left.r, 0.25 * (left.g + right.g) * 0.5, right.b);
    else if (m == 2u)    // green/magenta
        outc = float3(right.r, left.g, right.b);
    else                 // red/cyan
        outc = float3(left.r, right.g, right.b);

    // One alpha covers both eyes: the union of the shifted coverages.
    const float4 result = float4(outc, max(left4.a, right4.a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
