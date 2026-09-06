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
    float rate;
    float threshold;
    float pick;
    float viscosity;
    float angle;
    float spread;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;
[[vk::binding(3, 0)]] RWTexture2D<float4> state_out;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 dir = float2(cos(angle), sin(angle));
    const float2 canvas_size = float2(canvas_w, canvas_h);
    const float step_time = 60.0 / max(fps, 1.0);
    const float2 fall = dir * rate * step_time / canvas_size;

    const uint lane = uint(dot(uv * canvas_size, float2(dir.y, -dir.x)) + 4096.0);
    const float jitter = fx_hash01(fx_hash3(seed, lane, 0u)) - 0.5;
    const float2 lane_off =
        float2(dir.y, -dir.x) * jitter * spread * 2.0 * step_time / canvas_size;

    float4 trail =
        fx_sample_border(prev, uv - fall + lane_off);
    trail *= history_valid != 0 ? fx_decay(viscosity, fps) : 0.0;

    const float lum = fx_luma(fx_straight(base));
    const float gate = pick < 0.5 ? step(threshold, lum)
                                  : step(lum, 1.0 - threshold);
    const float coverage = base.a * gate;
    const float4 fed = float4(base.rgb, base.a) * gate;
    const float4 liquid = fed + trail * (1.0 - coverage);
    state_out[tid.xy] = liquid;

    float4 wet_px;
    wet_px = liquid + base * (1.0 - saturate(liquid.a));

    const float4 wetted = lerp(base, wet_px, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
