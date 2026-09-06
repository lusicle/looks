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
    const float dy = b * cos((1.0 - b) * 12.56637) * jump_px / canvas_h;
    const float2 suv = float2(uv.x, uv.y + dy);
    const float4 rolled = fx_sample_border(input, suv);
    float3 col = rolled.rgb;

    if (dy > 0.0005) {
        const float seam = 1.0 - dy;
        const float d = abs(uv.y - seam);
        const float bar = 1.0 - smoothstep(0.0, 6.0 / size.y, d);
        col *= 1.0 - bar * 0.85 * b;
    }

    col = lerp(col, float3(1.4, 1.35, 1.2) * rolled.a, flash * b * 0.7);

    if (dirt * b > 0.01) {
        const float2 pixel = uv * float2(canvas_w, canvas_h);
        const uint2 cell = uint2(pixel / 12.0);
        const uint h = fx_hash4(seed, 0u, cell.x, cell.y);
        if (fx_hash01(h) < dirt * b * 0.5) {
            const float2 cpos =
                (float2(cell) + float2(fx_hash01(fx_hash(h ^ 0x1u)),
                                       fx_hash01(fx_hash(h ^ 0x2u)))) * 12.0;
            const float2 d2 = pixel - cpos;
            const float rr = 1.5 + 3.0 * fx_hash01(fx_hash(h ^ 0x3u));
            if (dot(d2, d2) < rr * rr)
                col *= 0.15;
        }
    }

    const float4 result = float4(col, rolled.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
