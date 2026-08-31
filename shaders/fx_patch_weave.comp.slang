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
    float block;
    float candidates;
    float reach;
    float bias;
    float hold;
    float blend;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float block_score(float2 uv, float2 ref)
{
    float acc = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const float2 d = float2((i & 1) ? 0.25 : -0.25,
                                (i & 2) ? 0.25 : -0.25);
        const float2 a = clamp(uv + d, 0.0, 1.0);
        const float2 b = clamp(ref + d, 0.0, 1.0);
        acc += abs(fx_luma(input.SampleLevel(a, 0).rgb) -
                   fx_luma(input.SampleLevel(b, 0).rgb));
    }
    return acc * 0.25;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float bs = max(block, 2.0);
    const int2 cell = int2(floor(float2(tid.xy) / bs));
    const uint tick = hold >= 1.0 ? uint(float(frame) / max(hold, 1.0))
                                  : frame;

    const float2 cell_uv =
        (float2(tid.xy) - float2(cell) * bs + 0.5) / size;
    const float2 cell_origin = float2(cell) * bs / size;

    float2 best = cell_origin;
    float best_score = 1.0e9;
    const int n = int(clamp(candidates, 1.0, 8.0) + 0.5);
    for (int i = 0; i < n; ++i)
    {
        const uint h = fx_hash4(seed, uint(cell.x + 512), uint(cell.y + 512),
                                tick * 8u + uint(i));
        const float ax = fx_hash01(h) - 0.5;
        const float ay = fx_hash01(fx_hash(h)) - 0.5;
        const float2 cand =
            clamp(cell_origin + float2(ax, ay) * reach * 2.0, 0.0, 1.0);
        const float s = block_score(cell_origin + cell_uv, cand + cell_uv);
        const float score = lerp(s, float(n - i) * 1.0e-3, saturate(bias));
        if (score < best_score)
        {
            best_score = score;
            best = cand;
        }
    }

    const float2 src = clamp(best + cell_uv, 0.0, 1.0);
    float4 result = input.SampleLevel(src, 0);

    if (blend > 0.0)
    {
        const float2 f = abs(cell_uv * size / bs - 0.5) * 2.0;
        const float edge = saturate(max(f.x, f.y));
        const float k = smoothstep(1.0 - saturate(blend), 1.0, edge);
        result = lerp(result, base, k);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
