// bump drives the whole event and does nothing at 0, so it is meant to
// be mod-routed from a scene-cut or beat trigger.

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
    float bump;
    float jump_px;
    float flash;
    float dirt;
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

    const float b = saturate(bump);
    const float dy = b * jump_px / size.y;
    const float2 suv = float2(uv.x, frac(uv.y + dy));
    float3 col = input.SampleLevel(suv, 0).rgb;

    // Dark tape bar at the wrap seam.
    if (dy > 0.0005) {
        const float seam = 1.0 - dy;
        const float d = abs(uv.y - seam);
        const float bar = 1.0 - smoothstep(0.0, 6.0 / size.y, d);
        col *= 1.0 - bar * 0.85 * b;
    }

    col = lerp(col, float3(1.4, 1.35, 1.2), flash * b * 0.7);

    if (dirt * b > 0.01) {
        const uint2 cell = uint2(tid.xy / 12u);
        const uint h = fx_hash4(seed, frame, cell.x, cell.y);
        if (fx_hash01(h) < dirt * b * 0.5) {
            const float2 cpos =
                (float2(cell) + float2(fx_hash01(fx_hash(h ^ 0x1u)),
                                       fx_hash01(fx_hash(h ^ 0x2u)))) * 12.0;
            const float2 d2 = float2(tid.xy) - cpos;
            const float rr = 1.5 + 3.0 * fx_hash01(fx_hash(h ^ 0x3u));
            if (dot(d2, d2) < rr * rr)
                col *= 0.15;
        }
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
