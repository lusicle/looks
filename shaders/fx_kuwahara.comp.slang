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
    float radius;   // px, capped at 6 by the unrolled loops
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const int r = clamp(int(radius + 0.5), 1, 6);
    float4 sums[4] = {0.0, 0.0, 0.0, 0.0};
    float2 moments[4] = {0.0, 0.0, 0.0, 0.0};
    float counts[4] = {0.0, 0.0, 0.0, 0.0};
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            float4 s = input.SampleLevel(clamp(uv + float2(x, y) / float2(canvas_w, canvas_h), 0.0, 1.0), 0);
            float l = fx_luma(fx_srgb_oetf(saturate(fx_straight(s))));
            for (int q = 0; q < 4; ++q) {
                if (((q & 1) == 0 ? x <= 0 : x >= 0) &&
                    ((q & 2) == 0 ? y <= 0 : y >= 0)) {
                    sums[q] += s;
                    moments[q] += float2(l, l * l);
                    counts[q] += 1.0;
                }
            }
        }
    }
    float4 result = 0.0;
    float total = 0.0;
    for (int q = 0; q < 4; ++q) {
        float2 m = moments[q] / counts[q];
        float variance = max(m.y - m.x * m.x, 0.0);
        float weight = 1.0 / pow(1.0 + variance * 200.0, 2.0);
        result += sums[q] / counts[q] * weight;
        total += weight;
    }
    result /= total;
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
