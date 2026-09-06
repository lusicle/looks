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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float mode;
    float size;
    float corner;
    float year;
    float month;
    float day;
    float start_time;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float4 base = input.SampleLevel(uv, 0);
    float2 canvas = float2(canvas_w, canvas_h);
    float2 px = uv * canvas;
    float unit = max(size, 1.0);
    float2 f = canvas / float2(width, height) / unit;

    uint total_s = uint(float(frame) / max(fps, 1.0) + start_time);
    uint hh = (total_s / 3600u) % 24u;
    uint mm = (total_s / 60u) % 60u;
    uint ss = total_s % 60u;
    uint yy = uint(year + 0.5);
    uint mo = clamp(uint(month + 0.5), 1u, 12u);
    uint days[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    days[1] += yy % 4u == 0u && (yy % 100u != 0u || yy % 400u == 0u) ? 1u : 0u;
    uint dd = clamp(uint(day + 0.5), 1u, days[mo - 1u]) + total_s / 86400u;
    while (dd > days[mo - 1u]) {
        dd -= days[mo - 1u];
        if (++mo > 12u) {
            mo = 1u;
            ++yy;
            days[1] = 28u + (yy % 4u == 0u && (yy % 100u != 0u || yy % 400u == 0u) ? 1u : 0u);
        }
    }
    int date[16] = {int(mo / 10u), int(mo % 10u), -1, int(dd / 10u), int(dd % 10u),
                   -1, int(yy / 1000u % 10u), int(yy / 100u % 10u), int(yy / 10u % 10u), int(yy % 10u),
                   -1, -1, -1, -1, -1, -1};
    int clock[16] = {int(hh / 10u), int(hh % 10u), -2, int(mm / 10u), int(mm % 10u),
                    -2, int(ss / 10u), int(ss % 10u), -1, -1, -1, -1, -1, -1, -1, -1};
    uint m = uint(mode + 0.5);
    uint c = uint(corner + 0.5);
    uint lines = m == 2u ? 2u : 1u;
    float cov = 0.0;
    for (uint li = 0u; li < lines; ++li) {
        bool is_date = m == 0u || (m == 2u && li == 0u);
        uint slots = is_date ? 10u : 8u;
        float line_w = (float(slots) * 5.0 - 1.0) * unit;
        float x = (c & 1u) != 0u ? canvas.x - 3.0 * unit - line_w : 3.0 * unit;
        float y = (c & 2u) != 0u ? canvas.y - (10.0 + float(lines - 1u - li) * 9.0) * unit
                                 : (3.0 + float(li) * 9.0) * unit;
        float2 p = (px - float2(x, y)) / unit;
        cov = max(cov, is_date ? overlay_line(p, date, 10u, f) : overlay_line(p, clock, 8u, f));
    }
    float4 result = overlay_ink(base, float3(1.0, 0.55, 0.12), cov * 0.92);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
