[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float wet;
    float opacity;
};

[[vk::binding(0, 0)]] Sampler2D dry;
[[vk::binding(1, 0)]] Sampler2D fx;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 a = dry.SampleLevel(uv, 0);
    const float4 b = fx.SampleLevel(uv, 0);
    const float4 wetted = lerp(a, b, saturate(wet));
    const float4 mixed = lerp(a, wetted, saturate(opacity));
    // Dry alpha survives. The NV12 roundtrip brings back no alpha.
    output[tid.xy] = float4(mixed.rgb, a.a);
}
