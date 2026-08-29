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
    float gain;
    float pure_diff;
    // The fit rect is in canvas px. The luma planes use native pixels.
    float rect_x;
    float rect_y;
    float inv_fit_w;
    float inv_fit_h;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev_luma;
[[vk::binding(2, 0)]] Sampler2D cur_luma;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float2 luma_uv = (float2(tid.xy) + 0.5 - float2(rect_x, rect_y))
                         * float2(inv_fit_w, inv_fit_h);
    const bool inside = luma_uv.x >= 0.0 && luma_uv.x <= 1.0 &&
                        luma_uv.y >= 0.0 && luma_uv.y <= 1.0;
    const float cur = cur_luma.SampleLevel(luma_uv, 0).r;
    const float prev = prev_luma.SampleLevel(luma_uv, 0).r;
    const float diff =
        inside ? saturate(abs(cur - prev) * max(gain, 0.0)) : 0.0;

    const float3 diff_rgb = float3(diff, diff, diff);
    const float3 scaled = base.rgb * diff;
    const float4 result =
        float4(lerp(scaled, diff_rgb, saturate(pure_diff)), base.a);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
