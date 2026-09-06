#include "fx_optics.slang"
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
    float amount;
    float radius;
    float threshold;
    float mode;
    float smear;
};
[[vk::binding(0, 0)]] Sampler2D dry;
[[vk::binding(1, 0)]] Sampler2D filtered;
[[vk::binding(2, 0)]] Sampler2D columns;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float4 base = dry.SampleLevel(uv, 0);
    float4 result = base;
    float2 r = radius / float2(canvas_w, canvas_h);
    int m = int(mode + 0.5);
    if (amount > 0.0) {
        if (m == 0) {
            float4 spread = fx_scatter(filtered, uv, r);
            float l = fx_luma(base.rgb);
            float4 hot = base * max(l - threshold, 0.0) / max(l, 1e-5);
            float k = amount / (1.0 + amount);
            result = base + (spread - hot) * k;
            result.a = max(base.a, saturate(spread.a * k));
        } else if (m == 1) {
            float3 halo = fx_film_halo(filtered, uv, r, 0.0, 1.0) * amount;
            result.rgb += halo;
            result.a = max(base.a, saturate(fx_luma(halo)));
        } else {
            result = filtered.SampleLevel(uv, 0);
            float3 column = columns.SampleLevel(float2(uv.x, 0.5), 0).rgb *
                (smear * amount * 0.2);
            result.rgb = saturate(result.rgb + column);
            result.a = max(result.a, max(result.r, max(result.g, result.b)));
        }
    }
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
