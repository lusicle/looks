// This pass writes the AC bit count and the quantized DC for pass 2.

#include "mosh_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint pw;
    uint ph;
    uint plane;           // 0 = Y, 1 = U, 2 = V
    uint mode;
    uint rung_count;
    uint blocks_w;        // this plane's block grid width
    uint pad0;
    uint pad1;
    uint4 rungs_lo;       // quality ladder, descending
    uint4 rungs_hi;       // packed as uint4 for the std140 stride
};

[[vk::binding(0, 0)]] Sampler2D nv_y;
[[vk::binding(1, 0)]] Sampler2D nv_uv;
[[vk::binding(2, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> clean_pred;
[[vk::binding(3, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_dc;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_bits;

int fetch_src(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(pw) - 1, int(ph) - 1));
    const float2 uv = (float2(p) + 0.5) / float2(pw, ph);
    if (plane == 0)
        return int(nv_y.SampleLevel(uv, 0).r * 255.0 + 0.5);
    const float2 cbcr = nv_uv.SampleLevel(uv, 0).rg;
    return int((plane == 1 ? cbcr.x : cbcr.y) * 255.0 + 0.5);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint bx = tid.x;
    const uint by = tid.y;
    const uint bw = blocks_w;
    const uint bh = (ph + 7) / 8;
    if (bx >= bw || by >= bh)
        return;
    const int2 org = int2(int(bx) * 8, int(by) * 8);
    const bool filler = org.x >= int(pw) || org.y >= int(ph);

    if (filler)
    {
        for (uint r = 0; r < rung_count; ++r)
        {
            // Intra fillers cost 14 bits and use a transparent sentinel.
            // P fillers are zero blocks. They cost 13 bits and reset the chain.
            InterlockedAdd(rate_bits[int2(int(r), 0)],
                           mode == 0 ? 14u : 13u);
            rate_dc[int2(int(r * bw + bx), int(by))] =
                mode == 0 ? 0x80000000u : 0x40000000u;
        }
        return;
    }

    int coeffs[64];
    for (uint i = 0; i < 64; ++i)
    {
        const int2 p = org + int2(int(i & 7u), int(i >> 3));
        const int2 pc = clamp(p, int2(0, 0), int2(int(pw) - 1, int(ph) - 1));
        coeffs[i] = mode == 0 ? fetch_src(p) - 128
                              : fetch_src(p) - int(clean_pred[pc]);
    }
    for (uint row = 0; row < 8; ++row)
        fwd_pass8(coeffs, row * 8, 1);
    for (uint col = 0; col < 8; ++col)
        fwd_pass8(coeffs, col, 8);

    for (uint r = 0; r < rung_count; ++r)
    {
        const int quality =
            int(clamp(mosh_rung_at(rungs_lo, rungs_hi, r), 1u, 100u));
        const int scale = mosh_quant_scale(quality);
        int dc = 0;
        uint bits = 0;
        int last_nonzero = 0;
        int qz[64];
        for (uint i = 0; i < 64; ++i)
        {
            const int qv = mosh_quant_step(i, plane, scale);
            const int v = coeffs[i];
            const int mag = (abs(v) + qv / 2) / qv;
            qz[i] = v < 0 ? -mag : mag;
        }
        dc = qz[0];
        for (int i = 1; i < 64; ++i)
            if (qz[kZigzag[i]] != 0)
                last_nonzero = i;
        uint run = 0;
        for (int i = 1; i <= last_nonzero; ++i)
        {
            const int v = qz[kZigzag[i]];
            if (v == 0)
            {
                ++run;
                continue;
            }
            bits += ue_bits(run);
            bits += ue_bits(uint(abs(v)) - 1) + 1;
            run = 0;
        }
        bits += ue_bits(63u);   // end-of-block marker
        InterlockedAdd(rate_bits[int2(int(r), 0)], bits);
        rate_dc[int2(int(r * bw + bx), int(by))] =
            uint(dc + 65536);   // biased, sentinels use the high bits
    }
}
