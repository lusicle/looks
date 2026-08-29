// This pass draws the state only. rd_step runs the sim.

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
    float feed;     // rd_step uses feed, kill, steps and inject
    float kill;
    float steps;
    float inject;
    float ink;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D state;   // A = r, B = g
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float b = state.SampleLevel(uv, 0).g;
    const float body = smoothstep(0.15, 0.35, b);
    const float rim = smoothstep(0.08, 0.15, b) - body;

    float3 col;
    float col_a = base.a;
    if (ink < 0.5) {
        col = base.rgb * (1.0 - body * 0.92);
        col += rim * base.rgb * 0.8 + rim * 0.05;
    } else {
        col = base.rgb + body * (base.rgb * 2.5 + 0.08) + rim * 0.15;
        // The pattern's own light carries coverage past the content.
        col_a = max(col_a, saturate(body * 0.08 + rim * 0.15));
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
