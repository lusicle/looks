// The integer DCT must stay bit-identical to the CPU codec core.
// Quantize and dequantize is the decode. No entropy coding runs here.

#include "mosh_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint pw;              // this plane's pixel dims
    uint ph;
    uint plane;           // 0 = Y, 1 = U, 2 = V
    uint mode;
    uint quality;         // 1 to 100
    uint write_moshed;
    uint src_from_plane;
    uint corrupt_enable;
    float corrupt_prob;
    uint seed_frame;
    uint mb_shift;        // 1 for luma, 0 for chroma
    uint quality_from_qsel;
};

[[vk::binding(0, 0)]] Sampler2D nv_y;
[[vk::binding(1, 0)]] Sampler2D nv_uv;
[[vk::binding(2, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> src_plane;
[[vk::binding(3, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> clean_pred;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> moshed_pred;
[[vk::binding(5, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> out_clean;
[[vk::binding(6, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> out_moshed;
[[vk::binding(7, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> qsel;

// The unorm texture round-trips the byte exactly.
int fetch_src(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(pw) - 1, int(ph) - 1));
    if (src_from_plane != 0)
        return int(src_plane[p]);
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
    if (bx * 8 >= pw || by * 8 >= ph)
        return;
    const int2 org = int2(int(bx) * 8, int(by) * 8);

    const int q_eff = int(quality_from_qsel != 0
                              ? clamp(qsel[int2(0, 0)], 1u, 100u)
                              : quality);
    const int scale = mosh_quant_scale(q_eff);
    int q[64];
    for (uint i = 0; i < 64; ++i)
        q[i] = mosh_quant_step(i, plane, scale);

    int blk[64];
    if (mode == 0)
    {
        for (uint i = 0; i < 64; ++i)
            blk[i] = fetch_src(org + int2(int(i & 7u), int(i >> 3))) - 128;
    }
    else
    {
        for (uint i = 0; i < 64; ++i)
        {
            const int2 p = org + int2(int(i & 7u), int(i >> 3));
            const int2 pc = clamp(p, int2(0, 0),
                                  int2(int(pw) - 1, int(ph) - 1));
            blk[i] = fetch_src(p) - int(clean_pred[pc]);
        }
    }

    dct8x8(blk, true);
    for (uint i = 0; i < 64; ++i)
    {
        const int v = blk[i];
        const int mag = (abs(v) + q[i] / 2) / q[i];
        const int quantized = v < 0 ? -mag : mag;
        blk[i] = clamp(quantized * q[i], -32768, 32767);
    }
    dct8x8(blk, false);

    if (mode == 0)
    {
        for (uint i = 0; i < 64; ++i)
        {
            const int2 p = org + int2(int(i & 7u), int(i >> 3));
            if (p.x >= int(pw) || p.y >= int(ph))
                continue;
            const uint val = uint(clamp(blk[i] + 128, 0, 255));
            out_clean[p] = val;
            if (write_moshed != 0)
                out_moshed[p] = val;
        }
        return;
    }

    // Seeded macroblock corruption skips the moshed add.
    const uint mbx = bx >> mb_shift;
    const uint mby = by >> mb_shift;
    const bool corrupt =
        corrupt_enable != 0 &&
        fx_hash01(fx_hash(seed_frame ^ fx_hash(mby * 4096u + mbx))) <
            corrupt_prob;
    for (uint i = 0; i < 64; ++i)
    {
        const int2 p = org + int2(int(i & 7u), int(i >> 3));
        if (p.x >= int(pw) || p.y >= int(ph))
            continue;
        out_clean[p] = uint(clamp(int(clean_pred[p]) + blk[i], 0, 255));
        out_moshed[p] = corrupt
            ? moshed_pred[p]
            : uint(clamp(int(moshed_pred[p]) + blk[i], 0, 255));
    }
}
