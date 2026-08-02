// GPU mosh: the codec wire, one thread per 8x8 block of ONE plane per
// dispatch. Ports codec_core's fixed-point DCT (13-bit constants, int32
// accumulators, symmetric rounding) exactly - integer shader math is
// bit-deterministic, so preview and export stay identical. Entropy never
// runs here: quantize + dequantize IS the decode when no bitstream is
// rate-limited or corrupted (the CPU box keeps those paths).
//
// mode 0 = intra: source -> center -> DCT -> quant -> dequant -> IDCT ->
//   +128 into out_clean (and out_moshed when write_moshed).
// mode 1 = P: residual against clean_pred -> wire -> the SAME spatial
//   residual lands on both chains (corrupt-rolled macroblocks skip the
//   moshed add: prediction-only holes that persist - open loop).
// src_from_plane selects the src storage plane (generation-loss cycles)
// over the NV12 textures.

[[vk::push_constant]]
cbuffer Push
{
    uint pw;              // this plane's pixel dims
    uint ph;
    uint plane;           // 0 = Y (nv_y), 1 = U, 2 = V (nv_uv channels)
    uint mode;            // 0 intra, 1 P
    uint quality;         // 1..100
    uint write_moshed;    // intra: also write the moshed chain
    uint src_from_plane;  // 1 = read src storage plane, 0 = NV12 textures
    uint corrupt_enable;
    float corrupt_prob;
    uint seed_frame;
    uint mb_shift;        // block -> macroblock: 1 for luma, 0 for chroma
    uint quality_from_qsel;   // 1 = rate control picked it (read qsel)
};

[[vk::binding(0, 0)]] Sampler2D nv_y;
[[vk::binding(1, 0)]] Sampler2D nv_uv;
[[vk::binding(2, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> src_plane;
[[vk::binding(3, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> clean_pred;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> moshed_pred;
[[vk::binding(5, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> out_clean;
[[vk::binding(6, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> out_moshed;
[[vk::binding(7, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> qsel;

uint mosh_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float mosh_hash01(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

// ITU T.81 Annex K base tables (matches codec_core).
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

int round13(int acc)
{
    return clamp((acc + 4096) >> 13, -32768, 32767);
}

// Even/odd folded 8-point passes, bit-identical to codec_core's.
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

// Source fetch: NV12 textures (unorm round-trips the byte exactly) or the
// src storage plane, with edge replication for partial blocks.
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

    // Per-thread quant table: closed-form scale of the base table. Rate
    // control delivers its pick through qsel so the decision never leaves
    // the GPU.
    const int q_eff = int(quality_from_qsel != 0
                              ? clamp(qsel[int2(0, 0)], 1u, 100u)
                              : quality);
    const int scale = q_eff < 50 ? 5000 / q_eff : 200 - q_eff * 2;
    int q[64];
    for (uint i = 0; i < 64; ++i)
    {
        const int base = plane == 0 ? kQuantBaseLuma[i] : kQuantBaseChroma[i];
        q[i] = clamp((base * scale + 50) / 100, 1, 255);
    }

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

    // P frame: seeded per-macroblock corruption skips the moshed add.
    const uint mbx = bx >> mb_shift;
    const uint mby = by >> mb_shift;
    const bool corrupt =
        corrupt_enable != 0 &&
        mosh_hash01(mosh_hash(seed_frame ^ mosh_hash(mby * 4096u + mbx))) <
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
