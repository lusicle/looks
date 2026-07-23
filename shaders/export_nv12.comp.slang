// Export edge (spec §4): linear RGBA16F working space -> NV12 planes for
// the H.264 encoder MFT. OETF (sRGB encode, inverse of the import EOTF) +
// BT.709 limited-range matrix; chroma is the 2x2 block average computed in
// the encoded domain. One dispatch over full res: every thread writes Y,
// the even/even thread of each 2x2 block writes the interleaved UV texel.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> out_y;
[[vk::binding(2, 0)]] [[vk::image_format("rg8")]] RWTexture2D<float2> out_uv;

float3 srgb_oetf(float3 lin)
{
    const float3 hi = 1.055 * pow(lin, 1.0 / 2.4) - 0.055;
    const float3 lo = lin * 12.92;
    return lerp(hi, lo, step(lin, 0.0031308));
}

float luma709(float3 encoded)
{
    return dot(encoded, float3(0.2126, 0.7152, 0.0722));
}

float3 fetch_encoded(float2 texel, float2 size)
{
    const float2 uv = texel / size;
    return srgb_oetf(saturate(input.SampleLevel(uv, 0).rgb));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float3 encoded = fetch_encoded(float2(tid.xy) + 0.5, size);

    out_y[tid.xy] = (16.0 + 219.0 * luma709(encoded)) / 255.0;

    if ((tid.x & 1) == 0 && (tid.y & 1) == 0)
    {
        float3 sum = encoded;
        sum += fetch_encoded(float2(tid.xy) + float2(1.5, 0.5), size);
        sum += fetch_encoded(float2(tid.xy) + float2(0.5, 1.5), size);
        sum += fetch_encoded(float2(tid.xy) + float2(1.5, 1.5), size);
        const float3 avg = sum * 0.25;
        const float avg_y = luma709(avg);
        const float cb = (avg.b - avg_y) / 1.8556;
        const float cr = (avg.r - avg_y) / 1.5748;
        out_uv[tid.xy >> 1] = float2((128.0 + 224.0 * cb) / 255.0,
                                     (128.0 + 224.0 * cr) / 255.0);
    }
}
