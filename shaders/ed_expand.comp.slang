// Error diffusion upload expand: packed level indices (3x5 bits per
// pixel) to linear RGBA16F. The palette arrives via push, computed on the
// CPU with the exact tables the walk used - re-deriving it in-shader
// would drift by ulps.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint nlevels;
    uint pad0;
    float4 lv0;    // level_lin[0..3]
    float4 lv1;    // [4..7]
    float4 lv2;    // [8..11]
    float4 lv3;    // [12..15]
    float lv16;
};

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> packed;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outp;

float level_at(uint k)
{
    k = min(k, nlevels - 1);
    if (k < 4) return lv0[k];
    if (k < 8) return lv1[k - 4];
    if (k < 12) return lv2[k - 8];
    if (k < 16) return lv3[k - 12];
    return lv16;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const uint p = packed[tid.xy];
    outp[tid.xy] = float4(level_at(p & 31u), level_at((p >> 5) & 31u),
                          level_at((p >> 10) & 31u), 1.0);
}
