// External mask-source video fetch (spec §8): I420 planes -> linear RGB at
// the working resolution, with the fit policy for resolution mismatch —
// stretch, fill (aspect-preserving cover), or tile at native size (tile +
// a 2-second loop = an infinite animated texture).

[[vk::push_constant]]
cbuffer Push
{
    uint width;     // working target
    uint height;
    uint src_w;     // native source
    uint src_h;
    uint fit;       // 0 stretch, 1 fill, 2 tile
};

[[vk::binding(0, 0)]] Sampler2D plane_y;
[[vk::binding(1, 0)]] Sampler2D plane_u;
[[vk::binding(2, 0)]] Sampler2D plane_v;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

float3 srgb_eotf(float3 encoded)
{
    const float3 hi = pow((encoded + 0.055) / 1.055, 2.4);
    const float3 lo = encoded / 12.92;
    return lerp(hi, lo, step(encoded, 0.04045));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);

    float2 suv;
    if (fit == 2u) {
        // Tile at native pixel scale.
        suv = frac((float2(tid.xy) + 0.5) / float2(src_w, src_h));
    } else if (fit == 1u) {
        // Cover: preserve source aspect, crop the overflow.
        const float src_aspect = float(src_w) / float(src_h);
        const float dst_aspect = float(width) / float(height);
        suv = uv;
        if (src_aspect > dst_aspect)
            suv.x = (uv.x - 0.5) * dst_aspect / src_aspect + 0.5;
        else
            suv.y = (uv.y - 0.5) * src_aspect / dst_aspect + 0.5;
    } else {
        suv = uv;
    }

    const float y = plane_y.SampleLevel(suv, 0).r;
    const float cb = plane_u.SampleLevel(suv, 0).r;
    const float cr = plane_v.SampleLevel(suv, 0).r;

    const float yf = (y * 255.0 - 16.0) / 219.0;
    const float cbf = (cb * 255.0 - 128.0) / 224.0;
    const float crf = (cr * 255.0 - 128.0) / 224.0;

    float3 rgb;
    rgb.r = yf + 1.5748 * crf;
    rgb.g = yf - 0.1873 * cbf - 0.4681 * crf;
    rgb.b = yf + 1.8556 * cbf;
    rgb = saturate(rgb);

    output[tid.xy] = float4(srgb_eotf(rgb), 1.0);
}
