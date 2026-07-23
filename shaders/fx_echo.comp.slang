// Echo / ghost trails (spec §6.1 category 1): decay blend against the
// effect's own previous OUTPUT (persistent per-instance target fed by the
// engine). mode 0 = lighten trails (max), 1 = crossfade smear.
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
    float decay;       // trail persistence per frame
    float mode;        // 0 lighten, 1 crossfade
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev_out;   // this effect's last output
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float3 trail = prev_out.SampleLevel(uv, 0).rgb * saturate(decay);

    float3 echoed;
    if (int(mode + 0.5) == 0)
        echoed = max(base.rgb, trail);
    else
        echoed = lerp(base.rgb, trail, saturate(decay));

    const float4 result = float4(echoed, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
