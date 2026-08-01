// Camcorder HUD: blinking REC dot + label, battery gauge,
// tape counter off the timeline clock, safe-frame corner brackets. Drawn
// procedurally in encoded space (it is signal, not scene). Put this
// BEFORE a Codec-Box in the stack and the HUD gets JPEG-crushed with the
// picture — the authentic ordering.
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

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
    float size;       // px per unit
    float blink_hz;   // REC dot blink
    float elements;   // 0 rec, 1 +counter, 2 full (battery + brackets)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const uint kDigit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                0x6D, 0x7D, 0x07, 0x7F, 0x6F};

float seg_coverage(uint mask, float2 lp)
{
    if (lp.x < 0.0 || lp.x >= 4.0 || lp.y < 0.0 || lp.y >= 7.0) return 0.0;
    bool on = false;
    if ((mask & 0x01u) != 0u && lp.y < 1.0 && lp.x >= 0.5 && lp.x < 3.5)
        on = true;
    if ((mask & 0x40u) != 0u && lp.y >= 3.0 && lp.y < 4.0 &&
        lp.x >= 0.5 && lp.x < 3.5) on = true;
    if ((mask & 0x08u) != 0u && lp.y >= 6.0 && lp.x >= 0.5 && lp.x < 3.5)
        on = true;
    if ((mask & 0x20u) != 0u && lp.x < 1.0 && lp.y >= 0.5 && lp.y < 3.5)
        on = true;
    if ((mask & 0x02u) != 0u && lp.x >= 3.0 && lp.y >= 0.5 && lp.y < 3.5)
        on = true;
    if ((mask & 0x10u) != 0u && lp.x < 1.0 && lp.y >= 3.5 && lp.y < 6.5)
        on = true;
    if ((mask & 0x04u) != 0u && lp.x >= 3.0 && lp.y >= 3.5 && lp.y < 6.5)
        on = true;
    return on ? 1.0 : 0.0;
}

// Box helper in overlay units.
float box(float2 p, float2 lo, float2 hi)
{
    return (p.x >= lo.x && p.x < hi.x && p.y >= lo.y && p.y < hi.y) ? 1.0
                                                                    : 0.0;
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
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const int el = int(elements + 0.5);
    const float2 px = float2(tid.xy) + 0.5;

    float white = 0.0, red = 0.0;

    // REC dot + label, top-left.
    {
        const float2 origin = float2(6.0, 5.0) * unit;
        const float2 lp = (px - origin) / unit;
        const bool blink = blink_hz < 0.05 ||
                           frac(t * blink_hz) < 0.6;
        if (blink) {
            const float2 d = lp - float2(2.0, 3.5);
            if (dot(d, d) < 4.0) red = 1.0;
        }
        // "REC" as chunky segments: R=digit-ish boxes keeps it honest to
        // low-budget on-screen fonts: draw three 4x7 letter cells.
        // R: left bar + top + mid + diagonal-ish lower right stub.
        const float2 rp = lp - float2(6.0, 0.0);
        white = max(white, box(rp, float2(0, 0), float2(1, 7)));
        white = max(white, box(rp, float2(0.5, 0), float2(3.5, 1)));
        white = max(white, box(rp, float2(0.5, 3), float2(3.5, 4)));
        white = max(white, box(rp, float2(3, 0.5), float2(4, 3.5)));
        white = max(white, box(rp, float2(2.5, 4), float2(3.5, 7)));
        // E
        const float2 ep = lp - float2(11.5, 0.0);
        white = max(white, box(ep, float2(0, 0), float2(1, 7)));
        white = max(white, box(ep, float2(0.5, 0), float2(3.5, 1)));
        white = max(white, box(ep, float2(0.5, 3), float2(3.0, 4)));
        white = max(white, box(ep, float2(0.5, 6), float2(3.5, 7)));
        // C
        const float2 cp = lp - float2(17.0, 0.0);
        white = max(white, box(cp, float2(0, 0), float2(1, 7)));
        white = max(white, box(cp, float2(0.5, 0), float2(3.5, 1)));
        white = max(white, box(cp, float2(0.5, 6), float2(3.5, 7)));
    }

    // Counter (mm:ss), top-right.
    if (el >= 1) {
        const uint total_s = uint(t);
        const uint mm = (total_s / 60u) % 100u;
        const uint ss = total_s % 60u;
        const int codes[5] = {int(mm / 10u), int(mm % 10u), -2,
                              int(ss / 10u), int(ss % 10u)};
        const float line_w = (5.0 * 5.0 - 1.0) * unit;
        const float2 origin =
            float2(size_px.x - 6.0 * unit - line_w, 5.0 * unit);
        const float2 lp = (px - origin) / unit;
        if (lp.y >= 0.0 && lp.y < 7.0) {
            const int slot = int(floor(lp.x / 5.0));
            if (slot >= 0 && slot < 5) {
                const float2 cp = float2(lp.x - float(slot) * 5.0, lp.y);
                const int code = codes[slot];
                if (code >= 0)
                    white = max(white, seg_coverage(kDigit[code], cp));
                else if (cp.x >= 1.5 && cp.x < 2.5 &&
                         ((cp.y >= 1.5 && cp.y < 2.5) ||
                          (cp.y >= 4.5 && cp.y < 5.5)))
                    white = 1.0;
            }
        }
    }

    // Battery (bottom-left) + safe-frame brackets.
    if (el >= 2) {
        const float2 origin = float2(6.0, size_px.y - 10.0 * unit);
        const float2 lp = (px - origin) / unit;
        // Shell + tip + 2 of 3 bars filled.
        const float shell =
            max(box(lp, float2(0, 0), float2(12, 5)) -
                    box(lp, float2(0.7, 0.7), float2(11.3, 4.3)),
                box(lp, float2(12, 1.5), float2(13, 3.5)));
        white = max(white, shell);
        white = max(white, box(lp, float2(1.4, 1.4), float2(4.6, 3.6)));
        white = max(white, box(lp, float2(5.4, 1.4), float2(8.6, 3.6)));

        // Corner brackets at the 90% safe frame.
        const float2 m = size_px * 0.05;
        const float bl = 10.0 * unit;   // arm length
        const float bw = 1.0 * unit;    // arm width
        const float2 q = float2(min(px.x, size_px.x - px.x),
                                min(px.y, size_px.y - px.y));
        if ((abs(q.x - m.x) < bw * 0.5 && q.y >= m.y && q.y < m.y + bl) ||
            (abs(q.y - m.y) < bw * 0.5 && q.x >= m.x && q.x < m.x + bl))
            white = max(white, 0.8);
    }

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    enc = lerp(enc, float3(0.92, 0.92, 0.92), white * 0.9);
    enc = lerp(enc, float3(0.95, 0.15, 0.1), red * 0.95);

    const float4 result = float4(fx_srgb_eotf(saturate(enc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
