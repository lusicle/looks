#include "fx_crt_common.slang"

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D previous;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float2 c = uv * 2.0 - 1.0;
    float shift = converge * (0.25 + 0.375 * dot(c, c)) / canvas_width;
    float4 signal = 0.0;
    float total = 0.0;
    for (int i = -2; i <= 2; ++i) {
        float weight = exp(-0.5 * float(i * i));
        float2 p = uv + float2(float(i) * bandwidth / canvas_width, 0.0);
        float4 g = input.SampleLevel(p, 0);
        float4 r = shift > 0.0 ? input.SampleLevel(p - float2(shift, 0), 0) : g;
        float4 b = shift > 0.0 ? input.SampleLevel(p + float2(shift, 0), 0) : g;
        signal += float4(r.r, g.g, b.b, max(r.a, max(g.a, b.a))) * weight;
        total += weight;
        if (bandwidth <= 0.0) break;
    }
    signal /= total;
    if (interlaced >= 0.5) {
        float pixel_lines = float(height) * canvas_width / (canvas_height * float(width));
        float contrast = 1.0 - smoothstep(0.8, 1.0, pixel_lines);
        signal *= 1.0 + (((tid.y & 1u) == (frame & 1u)) ? contrast : -contrast);
    }
    if (history_valid != 0 && persistence > 0.0) {
        float3 decay = exp(-1000.0 / (max(fps, 1.0) * persistence *
                                      float3(0.75, 1.0, 0.55)));
        float4 prior = previous.SampleLevel(uv, 0);
        signal.rgb = lerp(signal.rgb, prior.rgb, decay);
        signal.a = lerp(signal.a, prior.a, max(decay.r, max(decay.g, decay.b)));
    }
    signal.a = saturate(signal.a);
    output[tid.xy] = signal;
}
