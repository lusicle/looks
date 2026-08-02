// Runs AFTER the effect kernel, which has already folded wet, blend and
// opacity, so this only chooses between dry and that finished result.

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
