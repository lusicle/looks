// Average the chroma in the encoded domain to invert the import path.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> out_y;
[[vk::binding(2, 0)]] [[vk::image_format("rg8")]] RWTexture2D<float2> out_uv;

float3 fetch_encoded(float2 texel, float2 size)
{
    const float2 uv = texel / size;
    return fx_srgb_oetf(saturate(input.SampleLevel(uv, 0).rgb));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float3 encoded = fetch_encoded(float2(tid.xy) + 0.5, size);

    out_y[tid.xy] = (16.0 + 219.0 * fx_luma(encoded)) / 255.0;

    if ((tid.x & 1) == 0 && (tid.y & 1) == 0)
    {
        float3 sum = encoded;
        sum += fetch_encoded(float2(tid.xy) + float2(1.5, 0.5), size);
        sum += fetch_encoded(float2(tid.xy) + float2(0.5, 1.5), size);
        sum += fetch_encoded(float2(tid.xy) + float2(1.5, 1.5), size);
        const float3 avg = sum * 0.25;
        const float2 cbcr = fx_rgb_to_cbcr709(avg, fx_luma(avg));
        out_uv[tid.xy >> 1] = (128.0 + 224.0 * cbcr) / 255.0;
    }
}
