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
    float period;      // seconds
    float dwell;       // frames
    float mark_size;   // radius, normalized to height
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

    const float fp = max(fps, 1.0);
    const float reel = max(period, 2.0);
    const float time = float(frame) / fp;
    const float first = max(reel - 8.0, 0.0);
    const float second = reel - 1.0;
    const float duration = max(dwell, 1.0) / 24.0;
    float exposure = 0.0;
    for (int cycle = 0; cycle < 2; ++cycle) {
        float start = floor(time / reel) * reel + float(cycle) * reel;
        exposure += max(min(time + 1.0 / fp, start + first + duration) - max(time, start + first), 0.0) * fp;
        exposure += max(min(time + 1.0 / fp, start + second + duration) - max(time, start + second), 0.0) * fp;
    }

    float mark = 0.0;
    if (exposure > 0.0) {
        const float2 center = float2(0.93, 0.09);
        const float2 d = (uv - center) * float2(size.x / size.y, 1.0);
        const float rr = length(d) / max(mark_size, 1.0e-3);
        const uint event = uint(time / reel);
        const float ang = atan2(d.y, d.x);
        const float wobble =
            0.85 + 0.25 * fx_value_noise1(seed ^ event, ang * 2.2 + 10.0);
        const float edge = 1.0 / (float(height) * max(mark_size, 1.0e-3));
        mark = (1.0 - smoothstep(wobble - edge, wobble + edge, rr)) * saturate(exposure);
    }

    const float alpha = mark * 0.9;
    const float4 result = float4(base.rgb * (1.0 - alpha) +
        fx_srgb_eotf(float3(0.10, 0.08, 0.07)) * alpha, base.a + alpha * (1.0 - base.a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
