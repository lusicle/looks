// The engine keys the strip on the frame index so export matches preview.

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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float window;    // the engine uses this, not the shader
    float mode;
    float thick;     // px
    float height_f;  // fraction of frame
    float pos_y;     // fraction of frame
    float glow;
    float color;
    float trigger;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D strip;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 mm = strip.SampleLevel(float2(uv.x, 0.5), 0).rg;
    const float mn = min(mm.x, mm.y);
    const float mx = max(mm.x, mm.y);

    const float y_c = saturate(pos_y) * size.y;
    const float half_px = clamp(height_f, 0.05, 1.0) * size.y * 0.5;
    const float py = float(tid.y);
    const float tk = max(thick * float(height) / canvas_h, 0.5);
    const uint m = uint(mode + 0.5);

    // Screen y grows downward, so max maps to y_top.
    float y_top, y_bot;
    if (m == 1u) {
        const float mag = max(abs(mn), abs(mx));
        y_top = y_c - mag * half_px;
        y_bot = y_c + mag * half_px;
    } else {
        y_top = y_c - mx * half_px;
        y_bot = y_c - mn * half_px;
    }

    float d_edge = min(abs(py - y_top), abs(py - y_bot));
    const bool inside = py >= y_top - 0.5 && py <= y_bot + 0.5;
    float trace = 0.0;
    if (m == 2u) {
        const float column = floor(uv.x * 1024.0);
        const float2 endpoints = strip.SampleLevel(float2((column + 0.5) / 1024.0, 0.5), 0).ba;
        const float2 a = float2(column * size.x / 1024.0, y_c - endpoints.x * half_px);
        const float2 b = float2((column + 1.0) * size.x / 1024.0, y_c - endpoints.y * half_px);
        d_edge = fx_segment_distance(float2(tid.xy) + 0.5, a, b);
        trace = exp(-pow(d_edge / tk, 2.0));
    } else {
        if (inside) trace = 0.28;
        trace += exp(-pow(d_edge / tk, 2.0)) * (inside ? 1.0 : 0.5);
    }
    const float d_c = abs(py - y_c);
    trace = max(trace, exp(-d_c * d_c * 0.8) * 0.15);

    const float3 tc =
        lerp(float3(0.3, 1.0, 0.5), base.rgb * 1.8 + 0.15,
             saturate(color));
    const float halo = exp(-pow(d_edge / (tk * 3.0), 2.0)) * saturate(glow) * 0.3;
    const float3 col = base.rgb + tc * (trace + halo) * 1.2;

    const float4 result =
        float4(col, max(base.a, saturate((trace + halo) * 1.2)));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
