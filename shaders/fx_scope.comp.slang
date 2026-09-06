#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push {
    uint width; uint height; float wet; float opacity; uint seed; uint frame; float fps;
    float canvas_w; float canvas_h; uint history_valid;
    float mode; float gain; float glow; float persist; float graticule; float color;
};
[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;
[[vk::binding(3, 0)]] RWTexture2D<float4> state_out;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> bins;

float3 trace_at(int2 p)
{
    if (any(p < 0) || any(p > 255)) return 0.0;
    float count = float(bins[p]);
    if (count <= 0.0) return 0.0;
    float3 sum = float3(bins[p + int2(256, 0)], bins[p + int2(512, 0)], bins[p + int2(768, 0)]);
    float3 tint = lerp(float3(0.25, 1.0, 0.45), sum / (count * 31.0), mode > 0.5 && mode < 1.5 ? 1.0 : saturate(color));
    float samples = float(width) * float(height) / 65536.0;
    float density = 1.0 - exp(-count / max(samples * 2.0, 1.0));
    return tint * density;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 size = float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) / size;
    float4 base = input.SampleLevel(uv, 0);
    float2 signal_uv = uv;
    if (mode > 1.5) signal_uv.x = (uv.x - 0.5) * float(width) / float(height) + 0.5;
    float2 pos = signal_uv * 255.0;
    int2 p = int2(floor(pos));
    float2 f = frac(pos);
    float3 trace = lerp(lerp(trace_at(p), trace_at(p + int2(1, 0)), f.x),
                        lerp(trace_at(p + int2(0, 1)), trace_at(p + int2(1, 1)), f.x), f.y);
    float3 halo = trace_at(p + int2(2, 0)) + trace_at(p - int2(2, 0)) +
                  trace_at(p + int2(0, 2)) + trace_at(p - int2(0, 2));
    trace += halo * saturate(glow) * 0.12;
    float3 col = max(trace, canvas.SampleLevel(uv, 0).rgb * fx_decay(persist, fps));
    state_out[tid.xy] = float4(col, saturate(fx_luma(col)));
    float grid = 0.0;
    if (mode < 1.5) {
        float level = frac((1.0 - uv.y) * 4.0);
        float d = min(level, 1.0 - level) * size.y * 0.25;
        grid = exp(-d * d * 2.0);
    } else {
        float2 ndc = (signal_uv - 0.5) * 2.0;
        float radius = length(ndc);
        float ring = exp(-pow((radius - 0.9) * size.y * 0.5, 2.0) * 0.5);
        float d = min(abs(ndc.x), abs(ndc.y)) * size.y * 0.5;
        grid = ring + exp(-d * d * 2.0) * step(radius, 0.9);
    }
    col += float3(0.25, 1.0, 0.45) * saturate(graticule) * grid * 0.1;
    float4 result = float4(col, max(base.a, saturate(fx_luma(col))));
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
