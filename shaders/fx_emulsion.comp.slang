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
    float warp;
    float mottle;
    float mold;
    float scale;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Bilinear 2D value noise on an integer lattice.
float vnoise2(uint s, float2 p)
{
    const float2 i = floor(p);
    const float2 f = p - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const int2 ip = int2(i) + int2(4096, 4096);
    const float a = fx_hash01(fx_hash4(s, uint(ip.x), uint(ip.y), 0u));
    const float b = fx_hash01(fx_hash4(s, uint(ip.x + 1), uint(ip.y), 0u));
    const float c = fx_hash01(fx_hash4(s, uint(ip.x), uint(ip.y + 1), 0u));
    const float d = fx_hash01(fx_hash4(s, uint(ip.x + 1), uint(ip.y + 1),
                                       0u));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float2 q = uv * scale * 3.0;
    const float drift = t * 0.07;

    // Two offset noise fields give a slow curl-like displacement.
    const float wx = vnoise2(seed ^ 0x11u, q + drift) - 0.5;
    const float wy = vnoise2(seed ^ 0x22u, q + 17.3 - drift) - 0.5;
    const float2 suv =
        clamp(uv + float2(wx, wy) * warp * 46.0 / size, 0.0, 1.0);
    float3 col = input.SampleLevel(suv, 0).rgb;

    // Decorrelated per channel, so layers fade at different rates.
    if (mottle > 0.001) {
        const float mr = vnoise2(seed ^ 0x33u, q * 1.7 + drift) - 0.5;
        const float mg = vnoise2(seed ^ 0x44u, q * 1.7 + 31.0) - 0.5;
        const float mb = vnoise2(seed ^ 0x55u, q * 1.7 - drift + 7.0) - 0.5;
        col *= 1.0 + float3(mr, mg * 0.6, mb) * mottle * 0.7;
    }

    // Dark olive blob centers with a bleached halo at the lifted edge.
    if (mold > 0.001) {
        const float n = vnoise2(seed ^ 0x66u, q * 2.3 + drift * 0.4) * 0.65 +
                        vnoise2(seed ^ 0x77u, q * 6.1) * 0.35;
        const float cut = 1.0 - mold * 0.45;
        const float body = smoothstep(cut, cut + 0.10, n);
        const float halo =
            smoothstep(cut - 0.08, cut, n) * (1.0 - body);
        const float l = fx_luma(col);
        col = lerp(col, float3(0.16, 0.20, 0.12) * (0.4 + l), body);
        col = lerp(col, col * 1.35 + 0.08, halo * 0.7);
    }

    const float4 result = float4(max(col, 0.0), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
