#ifndef FX_SEG7_SLANG
#define FX_SEG7_SLANG

static const uint kDigit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                0x6D, 0x7D, 0x07, 0x7F, 0x6F};

float overlay_box(float2 p, float2 lo, float2 hi, float2 footprint)
{
    float2 f = max(footprint, 1.0e-4);
    float2 covered = max(min(p + f * 0.5, hi) - max(p - f * 0.5, lo), 0.0) / f;
    return covered.x * covered.y;
}

float overlay_disc(float2 p, float radius, float2 footprint)
{
    float edge = max(length(footprint) * 0.5, 1.0e-4);
    return 1.0 - smoothstep(radius - edge, radius + edge, length(p));
}

float seg_coverage(uint mask, float2 p, float2 f)
{
    float c = 0.0;
    if ((mask & 0x01u) != 0u) c = max(c, overlay_box(p, float2(0.5, 0), float2(3.5, 1), f));
    if ((mask & 0x40u) != 0u) c = max(c, overlay_box(p, float2(0.5, 3), float2(3.5, 4), f));
    if ((mask & 0x08u) != 0u) c = max(c, overlay_box(p, float2(0.5, 6), float2(3.5, 7), f));
    if ((mask & 0x20u) != 0u) c = max(c, overlay_box(p, float2(0, 0.5), float2(1, 3.5), f));
    if ((mask & 0x02u) != 0u) c = max(c, overlay_box(p, float2(3, 0.5), float2(4, 3.5), f));
    if ((mask & 0x10u) != 0u) c = max(c, overlay_box(p, float2(0, 3.5), float2(1, 6.5), f));
    if ((mask & 0x04u) != 0u) c = max(c, overlay_box(p, float2(3, 3.5), float2(4, 6.5), f));
    return c;
}

float overlay_line(float2 p, int codes[16], uint count, float2 f)
{
    if (p.y < -f.y || p.y > 7.0 + f.y) return 0.0;
    int first = max(int(floor((p.x - f.x * 0.5) / 5.0)), 0);
    int last = min(int(floor((p.x + f.x * 0.5) / 5.0)), int(count) - 1);
    float c = 0.0;
    for (int slot = first; slot <= last; ++slot) {
        float2 q = p - float2(float(slot) * 5.0, 0.0);
        int code = codes[slot];
        if (code >= 0) c += seg_coverage(kDigit[min(code, 9)], q, f);
        else if (code == -2)
            c += overlay_box(q, float2(1.5, 1.5), float2(2.5, 2.5), f) +
                 overlay_box(q, float2(1.5, 4.5), float2(2.5, 5.5), f);
    }
    return saturate(c);
}

uint4 overlay_timecode(uint frame, float fps)
{
    uint nominal = max(uint(round(fps)), 1u);
    uint numbered = frame;
    if ((nominal == 30u || nominal == 60u) &&
        abs(fps - float(nominal) * 1000.0 / 1001.0) < 0.002) {
        uint drop = nominal / 15u;
        uint ten_minutes = nominal * 600u - drop * 9u;
        uint minutes = nominal * 60u - drop;
        uint blocks = frame / ten_minutes;
        uint remainder = frame % ten_minutes;
        numbered += drop * 9u * blocks;
        if (remainder >= drop) numbered += drop * ((remainder - drop) / minutes);
    }
    uint seconds = numbered / nominal;
    return uint4((seconds / 3600u) % 24u, (seconds / 60u) % 60u,
                 seconds % 60u, numbered % nominal);
}

float4 overlay_ink(float4 base, float3 ink, float coverage)
{
    float a = saturate(coverage);
    return float4(base.rgb * (1.0 - a) + fx_srgb_eotf(ink) * a,
                  base.a + a * (1.0 - base.a));
}
#endif
