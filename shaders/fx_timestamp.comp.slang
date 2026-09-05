// Draw in encoded space. The burn is an overlay on the signal.

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
    float size;    // px per segment unit
    float corner;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float colon_coverage(float2 lp)
{
    if (lp.x >= 1.5 && lp.x < 2.5 &&
        ((lp.y >= 1.5 && lp.y < 2.5) || (lp.y >= 4.5 && lp.y < 5.5)))
        return 1.0;
    return 0.0;
}

// codes: 0 to 9 is a digit, -1 is a space, -2 is a colon.
float line_coverage(float2 lp, int codes[10], uint count)
{
    if (lp.y < 0.0 || lp.y >= 7.0) return 0.0;
    const int slot = int(floor(lp.x / 5.0));
    if (slot < 0 || slot >= int(count)) return 0.0;
    const float2 cp = float2(lp.x - float(slot) * 5.0, lp.y);
    const int code = codes[slot];
    if (code >= 0) return seg_coverage(kDigit[code], cp);
    if (code == -2) return colon_coverage(cp);
    return 0.0;
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
    const float margin = 3.0 * unit;
    const uint m = uint(mode + 0.5);
    const uint c = uint(corner + 0.5);
    const bool right = (c & 1u) != 0u;
    const bool bottom = (c & 2u) != 0u;

    const uint total_s = fps > 0.0 ? uint(float(frame) / fps) : 0u;
    const uint hh = (total_s / 3600u) % 10u;
    const uint mm = (total_s / 60u) % 60u;
    const uint ss = total_s % 60u;
    int tc[10] = {int(hh), -2, int(mm / 10u), int(mm % 10u), -2,
                  int(ss / 10u), int(ss % 10u), -1, -1, -1};
    const uint tc_slots = 7u;
    // The date is fixed at 12 24 2003.
    int date[10] = {1, 2, -1, 2, 4, -1, 2, 0, 0, 3};
    const uint date_slots = 10u;

    float cov = 0.0;
    const uint lines = m == 2u ? 2u : 1u;
    for (uint li = 0u; li < lines; ++li) {
        const bool is_date = (m == 0u) || (m == 2u && li == 0u);
        const uint slots = is_date ? date_slots : tc_slots;
        const float line_w = (float(slots) * 5.0 - 1.0) * unit;
        const float x0 = right ? size_px.x - margin - line_w : margin;
        const float row = bottom ? float(li) : float(lines - 1u - li);
        const float y0 = bottom
            ? size_px.y - margin - 7.0 * unit - row * 9.0 * unit
            : margin + row * 9.0 * unit;
        const float2 lp =
            (float2(tid.xy) + 0.5 - float2(x0, y0)) / unit;
        if (is_date) cov = max(cov, line_coverage(lp, date, date_slots));
        else cov = max(cov, line_coverage(lp, tc, tc_slots));
    }

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float3 orange = float3(1.0, 0.55, 0.12);
    const float3 burned = lerp(enc, orange, cov * 0.92);

    const float4 result = float4(fx_srgb_eotf(saturate(burned)),
                                 max(base.a, cov * 0.92));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
