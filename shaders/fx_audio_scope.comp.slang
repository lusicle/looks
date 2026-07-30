// Audio Scope: the soundtrack's real waveform traced over the frame. The
// engine slices the mono PCM copy into a 1-D min/max strip covering the
// trailing `window` seconds ending at this timeline frame — deterministic
// on the frame index, so preview and export agree bit-exactly. No audio
// loaded = flat center line.
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
    float window;    // seconds shown (consumed engine-side for the strip)
    float mode;      // 0 filled, 1 mirror magnitude, 2 outline only
    float thick;     // edge thickness, px
    float height_f;  // trace height, fraction of frame
    float pos_y;     // trace center, fraction of frame
    float glow;      // brightness
    float color;     // 0 phosphor green -> 1 source color
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D strip;    // 1024x1 RG: (min, max) in -1..1
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
    const float tk = max(thick, 1.0);
    const uint m = uint(mode + 0.5);

    // Trace span for this column (screen y grows downward).
    float y_top, y_bot;
    if (m == 1u) {
        const float mag = max(abs(mn), abs(mx));
        y_top = y_c - mag * half_px;
        y_bot = y_c + mag * half_px;
    } else {
        y_top = y_c - mx * half_px;
        y_bot = y_c - mn * half_px;
    }

    // Edge distance to the span boundaries; inside-fill for modes 0/1.
    const float d_edge = min(abs(py - y_top), abs(py - y_bot));
    const bool inside = py >= y_top - 0.5 && py <= y_bot + 0.5;
    float trace = 0.0;
    if (m == 2u) {
        // Outline: light the whole min..max span thinly (the classic
        // connected waveform column).
        if (inside) trace = 0.9;
        trace = max(trace, exp(-pow(d_edge / tk, 2.0)) * 0.4);
    } else {
        if (inside) trace = 0.28;
        trace += exp(-pow(d_edge / tk, 2.0)) * (inside ? 1.0 : 0.5);
    }
    // Faint steady center line.
    const float d_c = abs(py - y_c);
    trace = max(trace, exp(-d_c * d_c * 0.8) * 0.15);

    const float3 tc =
        lerp(float3(0.3, 1.0, 0.5), base.rgb * 1.8 + 0.15,
             saturate(color));
    const float3 col = base.rgb + tc * trace * saturate(glow) * 1.2;

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
