// Mask channel combine (spec §8): fold a second mask into the first.
// add = union, subtract = cut, intersect = product (soft-edge friendly).

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint op;           // MaskCombineOp: 0 add, 1 subtract, 2 intersect
};

[[vk::binding(0, 0)]] Sampler2D mask_a;
[[vk::binding(1, 0)]] Sampler2D mask_b;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float a = mask_a.SampleLevel(uv, 0).r;
    const float b = mask_b.SampleLevel(uv, 0).r;
    float v;
    switch (op)
    {
    case 1: v = saturate(a - b); break;
    case 2: v = a * b; break;
    default: v = saturate(a + b); break;
    }
    output[tid.xy] = float4(v, v, v, 1.0);
}
