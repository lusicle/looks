// Pixelate/mosaic (proof effect, category 5). Block-averaged?
// No — classic point-sample at block centers (the crunchy look).
//
// Composition (canonical): wet is interior to the blend, opacity outside:
//   final = mix(input, blend(input, mix(input, fx(input), wet)), opacity)
// Blend is Normal (identity) until the compositing milestone.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float wet;
    float opacity;
    uint seed;        // shared fx prelude (unused here)
    uint frame;
    float fps;
    float block_size;   // px, >= 1
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
    const float4 base = input.SampleLevel(uv, 0);

    const float block = max(block_size, 1.0);
    const float2 block_uv =
        (floor(float2(tid.xy) / block) * block + block * 0.5) / size;
    const float4 result = input.SampleLevel(block_uv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
