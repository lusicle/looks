// Rutt-Etra (spec §6.6): the frame becomes horizontal scanlines displaced
// upward by luma — bright areas push their line up into a relief. For each
// output pixel, scan the candidate source scanlines below it and light up
// where a displaced line lands. Black elsewhere, line color from source.
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
    float spacing;   // px between scanlines
    float amount;    // max upward displacement, px
    float line_w;    // line half-thickness, px
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

    const float sp = max(spacing, 2.0);
    const float y = float(tid.y) + 0.5;

    // Scanlines whose source row lies in [y, y + amount] can reach us.
    const int first = int(ceil(y / sp)) - 1;
    const int last = int(floor((y + amount) / sp)) + 1;
    float3 col = 0.0;
    for (int s = first; s <= last; ++s) {
        if (s < 0) continue;
        const float row = float(s) * sp + sp * 0.5;
        if (row >= size.y) break;
        const float2 suv = float2(uv.x, (row + 0.5) / size.y);
        const float3 src = input.SampleLevel(suv, 0).rgb;
        const float l = fx_luma(fx_srgb_oetf(saturate(src)));
        const float displaced = row - l * amount;
        const float d = abs(y - displaced);
        if (d < line_w) {
            const float glow = 1.0 - d / line_w;
            col = max(col, src * (0.35 + 0.65 * l) * (glow * glow) * 2.0);
        }
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
