// Work in encoded space. This is a codec artifact, not scene light.

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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float amount;
    float radius;
    float rate;     // hz
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

    const float2 pixel = uv * float2(canvas_w, canvas_h);
    const float2 step_uv = max(radius, 1.0) / float2(canvas_w, canvas_h) * 0.5;
    const float3 encoded = fx_encoded(base);
    float3 filtered = 0.0;
    float weight = 0.0;
    for (int k = -4; k <= 4; ++k) {
        const float x = float(k) * 0.65;
        const float w = fx_sinc(x * 3.14159265) * fx_sinc(x * 1.04719755);
        const float3 horizontal = fx_encoded(input.SampleLevel(
            saturate(uv + float2(float(k) * step_uv.x, 0.0)), 0));
        const float3 vertical = fx_encoded(input.SampleLevel(
            saturate(uv + float2(0.0, float(k) * step_uv.y)), 0));
        filtered += (horizontal + vertical) * (w * 0.5);
        weight += w;
    }
    const float t = float(frame) / max(fps, 1.0);
    const uint tick = uint(t * max(rate, 0.01));
    const uint2 block = uint2(pixel / 8.0);
    const float strength = 0.75 + 0.25 * fx_hash01(fx_hash4(seed, tick, block.x, block.y));
    const float resolved = min(1.0, max(radius, 1.0) * float(width) / canvas_w);
    const float3 enc = saturate(encoded + (filtered / weight - encoded) * amount * strength * resolved);
    const float4 result = fx_from_encoded(enc, base.a);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
