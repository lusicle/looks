// This fixed-point wire must stay bit-identical to the CPU codec core.

#ifndef MOSH_COMMON_SLANG
#define MOSH_COMMON_SLANG

#include "fx_common.slang"

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

void inv_pass8(inout int blk[64], uint base, uint stride)
{
    const int v0 = blk[base + 0 * stride], v1 = blk[base + 1 * stride];
    const int v2 = blk[base + 2 * stride], v3 = blk[base + 3 * stride];
    const int v4 = blk[base + 4 * stride], v5 = blk[base + 5 * stride];
    const int v6 = blk[base + 6 * stride], v7 = blk[base + 7 * stride];
    const int e0 = 2896 * (v0 + v4) + 3784 * v2 + 1567 * v6;
    const int e1 = 2896 * (v0 - v4) + 1567 * v2 - 3784 * v6;
    const int e2 = 2896 * (v0 - v4) - 1567 * v2 + 3784 * v6;
    const int e3 = 2896 * (v0 + v4) - 3784 * v2 - 1567 * v6;
    const int o0 = 4017 * v1 + 3406 * v3 + 2276 * v5 + 799 * v7;
    const int o1 = 3406 * v1 - 799 * v3 - 4017 * v5 - 2276 * v7;
    const int o2 = 2276 * v1 - 4017 * v3 + 799 * v5 + 3406 * v7;
    const int o3 = 799 * v1 - 2276 * v3 + 3406 * v5 - 4017 * v7;
    blk[base + 0 * stride] = round13(e0 + o0);
    blk[base + 7 * stride] = round13(e0 - o0);
    blk[base + 1 * stride] = round13(e1 + o1);
    blk[base + 6 * stride] = round13(e1 - o1);
    blk[base + 2 * stride] = round13(e2 + o2);
    blk[base + 5 * stride] = round13(e2 - o2);
    blk[base + 3 * stride] = round13(e3 + o3);
    blk[base + 4 * stride] = round13(e3 - o3);
}

void dct8x8(inout int blk[64], bool forward)
{
    for (uint r = 0; r < 8; ++r)
    {
        if (forward) fwd_pass8(blk, r * 8, 1);
        else inv_pass8(blk, r * 8, 1);
    }
    for (uint c = 0; c < 8; ++c)
    {
        if (forward) fwd_pass8(blk, c, 8);
        else inv_pass8(blk, c, 8);
    }
}

int mosh_quant_scale(int quality)
{
    return quality < 50 ? 5000 / quality : 200 - quality * 2;
}

int mosh_quant_step(uint i, uint plane, int scale)
{
    const int base = plane == 0 ? kQuantBaseLuma[i] : kQuantBaseChroma[i];
    return clamp((base * scale + 50) / 100, 1, 255);
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

// The quality ladder uses two packed uint4s for std140 stride.
uint mosh_rung_at(uint4 lo, uint4 hi, uint r)
{
    return r < 4 ? lo[r] : hi[r - 4];
}

#endif
