// GPU mosh rate control, pass 1: one thread per 8x8 block of one plane.
// Transforms the block once, then for every quality rung on the ladder
// quantizes and accumulates the exact AC code length (run/level exp-Golomb
// + end-of-block) into bits[rung], and stores the quantized DC in the
// scratch image for pass 2's delta terms. The DC chain LOOKS serial but
// its size is a sum of pairwise terms - that is what makes rate control
// a reduction instead of a CPU job.

[[vk::push_constant]]
cbuffer Push
{
    uint pw;
    uint ph;
    uint plane;           // 0 = Y, 1 = U, 2 = V
    uint mode;            // 0 intra, 1 P residual
    uint rung_count;
    uint blocks_w;        // this plane's block grid width
    uint pad0;
    uint pad1;
    uint4 rungs_lo;       // quality ladder, descending (packed uint4s:
    uint4 rungs_hi;       // scalar arrays get 16-byte std140 strides)
};

uint rung_at(uint r)
{
    return r < 4 ? rungs_lo[r] : rungs_hi[r - 4];
}

[[vk::binding(0, 0)]] Sampler2D nv_y;
[[vk::binding(1, 0)]] Sampler2D nv_uv;
[[vk::binding(2, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> clean_pred;
[[vk::binding(3, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_dc;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_bits;

static const int kQuantBaseLuma[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99};

static const int kQuantBaseChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99};

static const uint kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

int round13(int acc)
{
    return clamp((acc + 4096) >> 13, -32768, 32767);
}

void fwd_pass8(inout int blk[64], uint base, uint stride)
{
    int s[4];
    int d[4];
    for (uint j = 0; j < 4; ++j)
    {
        const int a = blk[base + j * stride];
        const int b = blk[base + (7 - j) * stride];
        s[j] = a + b;
        d[j] = a - b;
    }
    int o[8];
    o[0] = round13(2896 * (s[0] + s[1] + s[2] + s[3]));
    o[4] = round13(2896 * ((s[0] + s[3]) - (s[1] + s[2])));
    o[2] = round13(3784 * (s[0] - s[3]) + 1567 * (s[1] - s[2]));
    o[6] = round13(1567 * (s[0] - s[3]) - 3784 * (s[1] - s[2]));
    o[1] = round13(4017 * d[0] + 3406 * d[1] + 2276 * d[2] + 799 * d[3]);
    o[3] = round13(3406 * d[0] - 799 * d[1] - 4017 * d[2] - 2276 * d[3]);
    o[5] = round13(2276 * d[0] - 4017 * d[1] + 799 * d[2] + 3406 * d[3]);
    o[7] = round13(799 * d[0] - 2276 * d[1] + 3406 * d[2] - 4017 * d[3]);
    for (uint k = 0; k < 8; ++k)
        blk[base + k * stride] = o[k];
}

uint ue_bits(uint v)
{
    const uint x = v + 1;
    uint bits = 0;
    while ((x >> bits) > 1)
        ++bits;
    return 2 * bits + 1;
}

uint se_bits(int v)
{
    return ue_bits(v > 0 ? uint(v) * 2 - 1 : uint(-v) * 2);
}

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
            // Intra fillers repeat the DC predictor: se(0) + EOB = 14
            // bits, chain-transparent (sentinel). P fillers are true zero
            // blocks: EOB only, DC 0 resets the chain.
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
        const int quality = int(clamp(rung_at(r), 1u, 100u));
        const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
        int dc = 0;
        uint bits = 0;
        int last_nonzero = 0;
        int qz[64];
        for (uint i = 0; i < 64; ++i)
        {
            const int base = plane == 0 ? kQuantBaseLuma[i]
                                        : kQuantBaseChroma[i];
            const int qv = clamp((base * scale + 50) / 100, 1, 255);
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
            uint(dc + 65536);   // biased; sentinels use high bits
    }
}
