// Binding 1 is this instance's output from the previous frame.

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
    float decay;
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev_out;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float4 prev = prev_out.SampleLevel(uv, 0);
    const float3 trail = prev.rgb * saturate(decay);
    const float trail_a = prev.a * saturate(decay);

    // The trail is content. Alpha decays the same as color.
    float3 echoed;
    float echoed_a;
    if (int(mode + 0.5) == 0) {
        echoed = max(base.rgb, trail);
        echoed_a = max(base.a, trail_a);
    } else {
        echoed = lerp(base.rgb, trail, saturate(decay));
        echoed_a = lerp(base.a, trail_a, saturate(decay));
    }

    const float4 result = float4(echoed, echoed_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
