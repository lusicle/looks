// I420 planes to linear RGB, BT.709 limited range. The transfer is
// decoded with the sRGB EOTF, which stands in for BT.709 camera gamma.
// The source lands in its aspect-preserving fit rect (rect origin +
// reciprocal size, output pixels); outside it the target is transparent
// black - sources never stretch to the canvas.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float rect_x;
    float rect_y;
    float inv_fit_w;
    float inv_fit_h;
    // NV12 chroma: plane_u is the interleaved RG8 CbCr texture (bound to
    // both chroma slots) - Cb in .r either way, Cr moves to plane_u.g.
    uint nv12;
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
    const float2 uv = (float2(tid.xy) + 0.5 - float2(rect_x, rect_y))
                    * float2(inv_fit_w, inv_fit_h);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        output[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float y = plane_y.SampleLevel(uv, 0).r;
    const float cb = plane_u.SampleLevel(uv, 0).r;
    const float cr = nv12 != 0 ? plane_u.SampleLevel(uv, 0).g
                               : plane_v.SampleLevel(uv, 0).r;

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
