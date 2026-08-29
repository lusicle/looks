// Operate in encoded space. The artifacts live in the signal.

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
    float mode;
    float res;           // virtual luma samples across the width
    float chroma_res;    // virtual chroma samples across the width
    float dot_crawl;
    float rainbow;
    float chroma_delay;  // px
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// The input RGB must be encoded, not linear.
float3 rgb_to_yiq(float3 c)
{
    return float3(dot(c, float3(0.299, 0.587, 0.114)),
                  dot(c, float3(0.5959, -0.2746, -0.3213)),
                  dot(c, float3(0.2115, -0.5227, 0.3112)));
}

float3 yiq_to_rgb(float3 yiq)
{
    return float3(yiq.x + 0.956 * yiq.y + 0.619 * yiq.z,
                  yiq.x - 0.272 * yiq.y - 0.647 * yiq.z,
                  yiq.x - 1.106 * yiq.y + 1.703 * yiq.z);
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

    const bool svideo = mode >= 0.5;
    const float smp = max(res, 32.0);
    // n0 and vline use virtual samples. Virtual pixels stay square.
    const float n0 = uv.x * smp;
    const uint vline = uint(uv.y * smp * size.y / size.x);
    // Carrier: 4 samples per cycle, 180 deg per line, 90 deg per frame.
    const float line_ph = 3.14159265 * float(vline & 1u) +
                          1.57079633 * float(frame & 3u);

    float y_sep = 0.0;
    float comp_c = 0.0;
    [unroll]
    for (int k = 0; k < 4; ++k) {
        const float n = n0 + (float(k) - 1.5);
        const float2 p = float2(n / smp, uv.y);
        const float3 yiq =
            rgb_to_yiq(fx_srgb_oetf(saturate(input.SampleLevel(p, 0).rgb)));
        const float th = 1.57079633 * n + line_ph;
        const float c = svideo
                            ? yiq.x
                            : yiq.x + yiq.y * cos(th) + yiq.z * sin(th);
        y_sep += c * 0.25;
        // One raw tap keeps full carrier amplitude for the crawl residue.
        if (k == 1) comp_c = c;
    }
    const float y_out = svideo
                            ? y_sep
                            : lerp(y_sep, comp_c, saturate(dot_crawl) * 0.6);

    const float cres = clamp(chroma_res, 8.0, smp);
    const float span = smp / cres * 2.0;    // window in virtual samples
    const float stride = span / 8.0;
    const float dxs = chroma_delay / size.x * smp;   // delay in samples
    float2 iq = float2(0.0, 0.0);
    float wsum = 0.0;
    [unroll]
    for (int k = 0; k < 9; ++k) {
        const float o = (float(k) - 4.0) * stride;
        const float n = n0 - dxs + o;
        const float2 p = float2(n / smp, uv.y);
        const float3 yiq =
            rgb_to_yiq(fx_srgb_oetf(saturate(input.SampleLevel(p, 0).rgb)));
        const float wgt = 1.0 - abs(float(k) - 4.0) / 5.0;
        if (svideo) {
            iq += yiq.yz * wgt;
        } else {
            const float th = 1.57079633 * n + line_ph;
            const float c = yiq.x + yiq.y * cos(th) + yiq.z * sin(th);
            const float feed = c - (1.0 - saturate(rainbow)) * yiq.x;
            iq += 2.0 * feed * float2(cos(th), sin(th)) * wgt;
        }
        wsum += wgt;
    }
    iq /= wsum;

    const float3 enc = saturate(yiq_to_rgb(float3(y_out, iq.x, iq.y)));
    const float4 result = float4(fx_srgb_eotf(enc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
