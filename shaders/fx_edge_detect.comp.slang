// Take the gradients in encoded space, not in linear light.

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
    float threshold;
    float mode;
    float thickness;   // sample radius, px
    float algo;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 e = thickness / size;
    float mag;
    if (algo >= 0.5) {
        float inner = 0.0, outer = 0.0;
        [unroll]
        for (int dy = -1; dy <= 1; ++dy)
            [unroll]
            for (int dx = -1; dx <= 1; ++dx) {
                const float2 o = float2(float(dx), float(dy));
                inner += fx_enc_luma(input, uv + o * e);
                outer += fx_enc_luma(input, uv + o * e * 2.6);
            }
        mag = abs(inner - outer) / 9.0 * 6.0;
    } else {
        const float tl = fx_enc_luma(input, uv + float2(-e.x, -e.y));
        const float tc = fx_enc_luma(input, uv + float2(0.0, -e.y));
        const float tr = fx_enc_luma(input, uv + float2(e.x, -e.y));
        const float ml = fx_enc_luma(input, uv + float2(-e.x, 0.0));
        const float mr = fx_enc_luma(input, uv + float2(e.x, 0.0));
        const float bl = fx_enc_luma(input, uv + float2(-e.x, e.y));
        const float bc = fx_enc_luma(input, uv + float2(0.0, e.y));
        const float br = fx_enc_luma(input, uv + float2(e.x, e.y));
        const float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
        const float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
        mag = length(float2(gx, gy));
    }
    const float line = smoothstep(threshold, threshold + 0.12, mag);

    const uint m = uint(mode + 0.5);
    float3 rgb;
    if (m == 0u) rgb = float3(line, line, line);
    else if (m == 1u) rgb = float3(1.0 - line, 1.0 - line, 1.0 - line);
    else rgb = base.rgb * (1.0 - line * 0.85);
    if (m != 2u) rgb = fx_srgb_eotf(rgb);

    const float4 result = float4(rgb, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
