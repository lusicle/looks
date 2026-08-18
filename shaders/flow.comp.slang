// One thread per 16x16 block, three-step SAD search, writing a per-block
// motion vector in CANVAS pixels to a (w/16, h/16) RG target. A pure
// function of the two luma planes, so flow never carries state across
// frames. The planes are NATIVE source pixels while the block grid is
// the canvas: sampling maps through the source's aspect-fit rect, the
// same placement ycbcr_to_rgb composites it at - blocks outside the fit
// rect are transparent bars and emit zero.

[[vk::push_constant]]
cbuffer Push
{
    uint width;        // working canvas size
    uint height;
    uint valid;        // 0 = seek discontinuity, emit zero flow
    float rect_x;      // the reference source's fit rect, canvas px
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
    // 8x8 taps at stride 2 over the 16x16 block; canvas px -> source uv
    // through the fit rect.
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

    // Sign convention: the search asks where the CURRENT block came FROM
    // in the previous frame, hence the minus on cand below.
    float2 mv = float2(0.0, 0.0);
    float best = block_sad(block_origin, block_origin, rect, inv_fit);
    for (int step = 8; step >= 1; step >>= 1)
    {
        bool improved = true;
        // Fixed round count: the search must not iterate to convergence,
        // or the result stops being deterministic across drivers.
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
