// Multiply in linear light. This is a surface, not a paint.

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
    float amount;
    float weave_px;   // thread pitch
    float hotspot;
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

    // The screen is static in screen space. It does not move.
    const float pitch = max(weave_px, 2.0);
    const float2 ph = float2(tid.xy) * (6.28318 / pitch);
    const float wv = 0.5 * (sin(ph.x) + sin(ph.y * 1.07 + 1.3));
    // Keep the per-cell irregularity or it looks like a grid.
    const uint2 cell = uint2(float2(tid.xy) / pitch);
    const float irregular =
        fx_hash01(fx_hash3(seed, cell.x, cell.y)) - 0.5;
    const float weave_mod =
        1.0 - saturate(amount) * 0.18 * (0.5 - 0.5 * wv + 0.4 * irregular);

    const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
    const float r2 = dot(d, d);
    const float spot = 1.0 + hotspot * (0.35 - r2 * 1.6);

    const float4 result =
        float4(base.rgb * weave_mod * max(spot, 0.2), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
