// Matte extraction: the wired image's luma IS the matte (masks ARE
// images — invert or shape it upstream with ordinary effects).

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float v =
        dot(input.SampleLevel(uv, 0).rgb, float3(0.2126, 0.7152, 0.0722));
    output[tid.xy] = float4(v, v, v, 1.0);
}
