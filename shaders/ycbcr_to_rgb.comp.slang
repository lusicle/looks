// I420 to linear RGB, BT.709 limited range, decoded with the sRGB EOTF.
// Outside the fit rect the output is transparent black. Never stretch.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float rect_x;
    float rect_y;
    float inv_fit_w;
    float inv_fit_h;
    // For NV12, plane_u is the interleaved RG8 CbCr texture in both slots.
    // Cb stays in .r and Cr moves to plane_u.g.
    uint nv12;
};

[[vk::binding(0, 0)]] Sampler2D plane_y;
[[vk::binding(1, 0)]] Sampler2D plane_u;
[[vk::binding(2, 0)]] Sampler2D plane_v;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

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
    rgb.r = yf + kFxCr709 * crf;
    rgb.g = yf - kFxCb709G * cbf - kFxCr709G * crf;
    rgb.b = yf + kFxCb709 * cbf;
    rgb = saturate(rgb);

    output[tid.xy] = float4(fx_srgb_eotf(rgb), 1.0);
}
