// Output motion vectors are in canvas pixels on a (w/16, h/16) grid.
// Luma planes use native source pixels. The fit rect maps canvas to uv.

[[vk::push_constant]]
cbuffer Push
{
    uint width;        // canvas size, not the plane size
    uint height;
    uint valid;
    float rect_x;      // source fit rect, canvas px
    float rect_y;
    float inv_fit_w;
    float inv_fit_h;
};

[[vk::binding(0, 0)]] Sampler2D cur_luma;
[[vk::binding(1, 0)]] Sampler2D prev_luma;
[[vk::binding(2, 0)]] RWTexture2D<float4> flow_out;

float block_sad(float2 cur_base, float2 prev_base, float2 rect,
                float2 inv_fit)
{
    float sad = 0.0;
    [unroll]
    for (int y = 0; y < 8; ++y)
    {
        [unroll]
        for (int x = 0; x < 8; ++x)
        {
            const float2 o = float2(x * 2 + 1, y * 2 + 1);
            const float a =
                cur_luma.SampleLevel((cur_base + o - rect) * inv_fit, 0).r;
            const float b =
                prev_luma.SampleLevel((prev_base + o - rect) * inv_fit, 0).r;
            sad += abs(a - b);
        }
    }
    return sad;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint bw = (width + 15) / 16;
    const uint bh = (height + 15) / 16;
    if (tid.x >= bw || tid.y >= bh)
        return;
    const float2 rect = float2(rect_x, rect_y);
    const float2 inv_fit = float2(inv_fit_w, inv_fit_h);
    const float2 block_origin = float2(tid.xy) * 16.0;
    const float2 center_uv = (block_origin + 8.0 - rect) * inv_fit;
    if (valid == 0 || center_uv.x < 0.0 || center_uv.x > 1.0 ||
        center_uv.y < 0.0 || center_uv.y > 1.0)
    {
        flow_out[tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // The search finds where the block came from. Thus the minus on cand.
    float2 mv = float2(0.0, 0.0);
    float best = block_sad(block_origin, block_origin, rect, inv_fit);
    for (int step = 8; step >= 1; step >>= 1)
    {
        bool improved = true;
        // A fixed round count keeps the result the same on all drivers.
        for (int round = 0; round < 2 && improved; ++round)
        {
            improved = false;
            [unroll]
            for (int d = 0; d < 4; ++d)
            {
                const float2 dirs[4] = {float2(1, 0), float2(-1, 0),
                                        float2(0, 1), float2(0, -1)};
                const float2 cand = mv + dirs[d] * float(step);
                if (abs(cand.x) > 24.0 || abs(cand.y) > 24.0)
                    continue;
                const float sad = block_sad(block_origin,
                                            block_origin - cand, rect,
                                            inv_fit);
                if (sad < best - 1.0e-5)
                {
                    best = sad;
                    mv = cand;
                    improved = true;
                }
            }
        }
    }
    flow_out[tid.xy] = float4(mv, 0.0, 1.0);
}
