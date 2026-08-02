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
    float shift_x;    // px; red shifts one way, blue the other
    float shift_y;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float2 offset = float2(shift_x, shift_y) / size;

    const float4 base = input.SampleLevel(uv, 0);
    float4 split = base;
    split.r = input.SampleLevel(uv + offset, 0).r;
    split.b = input.SampleLevel(uv - offset, 0).b;

    const float4 wetted = lerp(base, split, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
