// Draw in encoded space, not in linear light.

#include "fx_common.slang"
#include "fx_seg7.slang"

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
    float size;      // px per glyph unit
    float counter;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

// Right-pointing wedge in a 0..7 x 0..7 cell.
float wedge_cov(float2 lp)
{
    if (lp.x < 0.5 || lp.x >= 6.5 || lp.y < 0.0 || lp.y >= 7.0) return 0.0;
    const float half_h = 3.5 * (1.0 - (lp.x - 0.5) / 6.0);
    return abs(lp.y - 3.5) <= half_h ? 1.0 : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size_px = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size_px;
    const float4 base = input.SampleLevel(uv, 0);

    const float unit = max(size, 1.0);
    const float margin = 4.0 * unit;
    const uint m = uint(mode + 0.5);
    float cov = 0.0;
    float3 ink = float3(0.92, 0.92, 0.92);

    {
        const float2 lp =
            (float2(tid.xy) + 0.5 - float2(margin, margin)) / unit;
        if (m == 0u) {
            cov = max(cov, wedge_cov(lp));
        } else if (m == 1u) {
            const float t = fps > 0.0 ? float(frame) / fps : 0.0;
            if (frac(t) < 0.6) {
                const float2 d = lp - float2(3.5, 3.5);
                if (dot(d, d) < 6.5) {
                    cov = 1.0;
                    ink = float3(0.95, 0.15, 0.1);
                }
            }
        } else if (m == 2u) {
            if (lp.y >= 0.5 && lp.y < 6.5 &&
                ((lp.x >= 1.0 && lp.x < 2.8) || (lp.x >= 4.2 && lp.x < 6.0)))
                cov = 1.0;
        } else {
            cov = max(wedge_cov(lp), wedge_cov(lp - float2(4.5, 0.0)));
        }
    }

    // Slot code -2 is the colon in the M:SS:FF counter.
    if (counter >= 0.5) {
        const uint total_s = fps > 0.0 ? uint(float(frame) / fps) : 0u;
        const uint mm = (total_s / 60u) % 10u;
        const uint ss = total_s % 60u;
        const uint ff = fps > 0.0
            ? uint(fmod(float(frame), max(fps, 1.0))) : 0u;
        int codes[8] = {int(mm), -2, int(ss / 10u), int(ss % 10u), -2,
                        int((ff / 10u) % 10u), int(ff % 10u), -1};
        const uint slots = 7u;
        const float line_w = (float(slots) * 5.0 - 1.0) * unit;
        const float x0 = size_px.x - margin - line_w;
        const float2 lp =
            (float2(tid.xy) + 0.5 - float2(x0, margin)) / unit;
        if (lp.y >= 0.0 && lp.y < 7.0) {
            const int slot = int(floor(lp.x / 5.0));
            if (slot >= 0 && slot < int(slots)) {
                const float2 cp = float2(lp.x - float(slot) * 5.0, lp.y);
                const int code = codes[slot];
                float c = 0.0;
                if (code >= 0) c = seg_coverage(kDigit[code], cp);
                else if (code == -2 &&
                         cp.x >= 1.5 && cp.x < 2.5 &&
                         ((cp.y >= 1.5 && cp.y < 2.5) ||
                          (cp.y >= 4.5 && cp.y < 5.5)))
                    c = 1.0;
                if (c > cov) {
                    cov = c;
                    ink = float3(0.92, 0.92, 0.92);
                }
            }
        }
    }

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float3 burned = lerp(enc * (1.0 - cov * 0.35), ink, cov * 0.95);

    const float4 result = float4(fx_srgb_eotf(saturate(burned)),
                                 max(base.a, cov * 0.95));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
