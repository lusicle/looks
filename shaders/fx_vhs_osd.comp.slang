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
    float counter;
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

    float2 p = (px - 4.0 * unit) / unit;
    uint m = uint(mode + 0.5);
    float cov = 0.0;
    float3 ink = float3(0.92, 0.92, 0.92);
    if (m == 0u || m == 3u) {
        for (uint i = 0u; i < (m == 3u ? 2u : 1u); ++i) {
            float2 q = p - float2(float(i) * 4.5, 0.0);
            float d = max(0.5 - q.x, (abs(q.y - 3.5) + (q.x - 0.5) * 3.5 / 6.0 - 3.5) / 1.158);
            float edge = max(length(f) * 0.5, 1.0e-4);
            cov = max(cov, 1.0 - smoothstep(-edge, edge, d));
        }
    } else if (m == 1u) {
        if (frac(float(frame) / max(fps, 1.0)) < 0.6) {
            cov = overlay_disc(p - 3.5, 2.55, f);
            ink = float3(0.95, 0.15, 0.1);
        }
    } else {
        cov = max(overlay_box(p, float2(1.0, 0.5), float2(2.8, 6.5), f),
                  overlay_box(p, float2(4.2, 0.5), float2(6.0, 6.5), f));
    }
    float4 result = overlay_ink(base, ink, cov * 0.95);
    if (counter >= 0.5) {
        uint4 tc = overlay_timecode(frame, fps);
        int codes[16] = {int(tc.x / 10u), int(tc.x % 10u), -2,
                         int(tc.y / 10u), int(tc.y % 10u), -2,
                         int(tc.z / 10u), int(tc.z % 10u), -2,
                         int(tc.w / 10u), int(tc.w % 10u), -1, -1, -1, -1, -1};
        float2 origin = float2(canvas.x - 58.0 * unit, 4.0 * unit);
        float c = overlay_line((px - origin) / unit, codes, 11u, f);
        result = overlay_ink(result, float3(0.92, 0.92, 0.92), c * 0.95);
    }
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
