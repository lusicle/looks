// Anaglyph 3D: red/cyan (or red/blue, green/magenta) stereo double image.
// Disparity rides a luma depth proxy — bright subjects read as near and
// pop out of the screen harder than the shadows behind them.
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

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
    float pop;      // luma-driven extra disparity
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

    const float3 left =
        input.SampleLevel(clamp(uv - shift, 0.0, 1.0), 0).rgb;
    const float3 right =
        input.SampleLevel(clamp(uv + shift, 0.0, 1.0), 0).rgb;

    const uint m = uint(mode + 0.5);
    float3 outc;
    if (m == 1u)         // red/blue: green channel mostly dies
        outc = float3(left.r, 0.25 * (left.g + right.g) * 0.5, right.b);
    else if (m == 2u)    // green/magenta
        outc = float3(right.r, left.g, right.b);
    else                 // red/cyan
        outc = float3(left.r, right.g, right.b);

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
