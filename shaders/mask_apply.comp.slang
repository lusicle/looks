// Masked composite (spec §8): the effect's composed output replaces the
// dry input only where the mask is white. Runs after the effect kernel
// (which already folded wet/blend/opacity), closing the DAG diamond:
//   dry ──┬── fx ──┐
//         └────────┴─ apply(dry, fx, mask)

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] Sampler2D dry;
[[vk::binding(1, 0)]] Sampler2D wet_fx;
[[vk::binding(2, 0)]] Sampler2D mask;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 a = dry.SampleLevel(uv, 0);
    const float4 b = wet_fx.SampleLevel(uv, 0);
    const float m = mask.SampleLevel(uv, 0).r;
    output[tid.xy] = lerp(a, b, saturate(m));
}
