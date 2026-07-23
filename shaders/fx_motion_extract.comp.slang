// Motion extraction (spec §6.1 category 1): frame differencing against the
// previous source frame. gain lifts the difference; mix blends between
// difference-only (1) and difference-tinted source (0).
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
    float gain;        // difference amplification
    float pure_diff;   // 0 = source scaled by motion, 1 = raw difference
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev_luma;   // R8 previous frame Y (encoded)
[[vk::binding(2, 0)]] Sampler2D cur_luma;    // R8 current frame Y
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float cur = cur_luma.SampleLevel(uv, 0).r;
    const float prev = prev_luma.SampleLevel(uv, 0).r;
    const float diff = saturate(abs(cur - prev) * max(gain, 0.0));

    const float3 diff_rgb = float3(diff, diff, diff);
    const float3 scaled = base.rgb * diff;
    const float4 result =
        float4(lerp(scaled, diff_rgb, saturate(pure_diff)), base.a);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
