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
    const float2 fall = dir * rate / size;

    const uint lane = uint(dot(float2(tid.xy), float2(dir.y, -dir.x)) + 4096.0);
    const float jitter = fx_hash01(fx_hash3(seed, lane, 0u)) - 0.5;
    const float2 lane_off =
        float2(dir.y, -dir.x) * jitter * spread * 2.0 / size;

    float4 trail =
        prev.SampleLevel(clamp(uv - fall + lane_off, 0.0, 1.0), 0);
    trail *= saturate(viscosity);

    const float lum = fx_luma(base.rgb);
    const float gate = pick < 0.5 ? step(threshold, lum)
                                  : step(lum, 1.0 - threshold);
    const float4 fed = base * gate;

    float4 wet_px;
    wet_px.rgb = max(base.rgb, max(trail.rgb, fed.rgb));
    wet_px.a = max(base.a, max(trail.a, fed.a));

    const float4 wetted = lerp(base, wet_px, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
