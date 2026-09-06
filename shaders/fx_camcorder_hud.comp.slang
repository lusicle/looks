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
    float size;
    float blink_hz;
    float elements;
    float battery;
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

    float2 p = (px - float2(6.0, 5.0) * unit) / unit;
    float t = float(frame) / max(fps, 1.0);
    float red = blink_hz < 0.05 || frac(t * blink_hz) < 0.6
        ? overlay_disc(p - float2(2.0, 3.5), 2.0, f) : 0.0;
    float white = 0.0;
    float2 r = p - float2(6.0, 0.0);
    white = max(white, seg_coverage(0x73u, r, f));
    white = max(white, overlay_box(r, float2(2.5, 4), float2(3.5, 7), f));
    white = max(white, seg_coverage(0x79u, p - float2(11.5, 0.0), f));
    white = max(white, seg_coverage(0x39u, p - float2(17.0, 0.0), f));
    if (elements >= 0.5) {
        uint seconds = uint(t);
        uint hh = (seconds / 3600u) % 24u;
        uint mm = (seconds / 60u) % 60u;
        uint ss = seconds % 60u;
        int codes[16] = {int(hh / 10u), int(hh % 10u), -2, int(mm / 10u), int(mm % 10u),
                         -2, int(ss / 10u), int(ss % 10u), -1, -1, -1, -1, -1, -1, -1, -1};
        float2 origin = float2(canvas.x - 45.0 * unit, 5.0 * unit);
        white = max(white, overlay_line((px - origin) / unit, codes, 8u, f));
    }
    if (elements >= 1.5) {
        float2 b = (px - float2(6.0 * unit, canvas.y - 10.0 * unit)) / unit;
        float shell = max(overlay_box(b, float2(0, 0), float2(12, 5), f) -
                          overlay_box(b, float2(0.7, 0.7), float2(11.3, 4.3), f),
                          overlay_box(b, float2(12, 1.5), float2(13, 3.5), f));
        white = max(white, shell);
        white = max(white, overlay_box(b, float2(1.4, 1.4),
                                      float2(1.4 + 9.2 * saturate(battery), 3.6), f));
        float2 q = min(px, canvas - px) / unit;
        float2 margin = canvas * 0.05 / unit;
        float brackets = max(overlay_box(q, margin - float2(0.5, 0), margin + float2(0.5, 10), f),
                             overlay_box(q, margin - float2(0, 0.5), margin + float2(10, 0.5), f));
        white = max(white, brackets * 0.8);
    }
    float4 result = overlay_ink(base, float3(0.92, 0.92, 0.92), white * 0.9);
    result = overlay_ink(result, float3(0.95, 0.15, 0.1), red * 0.95);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
