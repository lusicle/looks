// GPU mosh rate control, pass 2: the DC delta code lengths. Every block's
// term depends only on its own quantized DC and its predecessor's in the
// STREAM traversal order (luma: macroblock raster, four blocks row-major
// inside each; chroma: block raster) - a pairwise map, not a chain.
// Intra edge fillers repeat the predictor (chain-transparent, bounded
// lookback); P fillers are zero blocks that reset it.

#include "mosh_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint pw;
    uint ph;
    uint plane;
    uint mode;
    uint rung_count;
    uint blocks_w;
};

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_dc;
[[vk::binding(1, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_bits;

// Sequence index -> block coordinates for this plane's traversal.
int2 seq_to_block(uint seq, uint mb_w)
{
    if (plane != 0)
        return int2(int(seq % blocks_w), int(seq / blocks_w));
    const uint mb = seq / 4;
    const uint b = seq % 4;
    return int2(int((mb % mb_w) * 2 + (b & 1u)),
                int((mb / mb_w) * 2 + (b >> 1)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint bw = blocks_w;
    const uint bh = (ph + 7) / 8;
    if (tid.x >= bw || tid.y >= bh)
        return;
    const uint mb_w = plane == 0 ? (bw + 1) / 2 : bw;
    const uint seq = plane == 0
        ? ((tid.y >> 1) * mb_w + (tid.x >> 1)) * 4 +
              ((tid.y & 1u) << 1) + (tid.x & 1u)
        : tid.y * bw + tid.x;

    for (uint r = 0; r < rung_count; ++r)
    {
        const uint cur = rate_dc[int2(int(r * bw + tid.x), int(tid.y))];
        if (cur == 0x80000000u)
            continue;   // intra filler: fixed bits counted in pass 1
        const int dc = cur == 0x40000000u ? 0 : int(cur) - 65536;
        int pred = 0;
        uint back = seq;
        for (uint step = 0; step < 8 && back > 0; ++step)
        {
            back -= 1;
            const int2 pb = seq_to_block(back, mb_w);
            const uint pv = rate_dc[int2(int(r * bw + uint(pb.x)),
                                         pb.y)];
            if (pv == 0x80000000u)
                continue;   // transparent: keep looking back
            pred = pv == 0x40000000u ? 0 : int(pv) - 65536;
            break;
        }
        InterlockedAdd(rate_bits[int2(int(r), 0)], se_bits(dc - pred));
    }
}
