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
    float amount;      // px of travel
    float speed;       // hz
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float3 camera_pose(float seconds, float response)
{
    float3 omega = speed * 6.2831853 * float3(1.0, 0.73, 1.37);
    float3 phase = 6.2831853 * float3(fx_hash01(seed), fx_hash01(seed ^ 0x5555u),
                                    fx_hash01(seed ^ 0x9999u));
    float3 lag = omega * response;
    float3 fundamental = sin(omega * seconds + phase - atan(lag)) / sqrt(1.0 + lag * lag);
    lag *= 2.0;
    float3 harmonic = sin(omega * seconds * 2.0 + phase * 1.7 - atan(lag)) / sqrt(1.0 + lag * lag);
    return amount * (fundamental * 0.75 + harmonic * 0.25);
}

float2 camera_uv(float2 uv, float3 pose, float zoom)
{
    float2 canvas = float2(canvas_w, canvas_h);
    float2 p = (uv - 0.5) * canvas / zoom;
    float roll = pose.z / max(canvas_w, canvas_h);
    float c = cos(roll), s = sin(roll);
    p = float2(c * p.x - s * p.y, s * p.x + c * p.y);
    return (p + pose.xy) / canvas + 0.5;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);
    if (amount <= 0.0 || wet <= 0.0 || opacity <= 0.0) { output[tid.xy] = base; return; }

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const int m = int(mode + 0.5);
    if (m == 1 || m == 3) {
        float dt = 1.0 / max(fps, 1.0);
        float readout = min(dt, 1.0 / 60.0) * 0.9;
        float3 correction = m == 1 ? camera_pose(t - 0.005, 0.012) - camera_pose(t, 0.18) :
            float3(0.0, 0.0, 0.0);
        float zoom = m == 1 ? 1.0 + 4.0 * amount / min(canvas_w, canvas_h) : 1.0;
        float travel = amount * speed * dt * 6.2831853 * max(width / canvas_w, height / canvas_h);
        uint taps = uint(clamp(ceil(travel), 1.0, 128.0));
        float4 result = 0.0;
        for (uint i = 0u; i < taps; ++i) {
            float sample_time = t + (uv.y - 0.5) * readout +
                ((float(i) + 0.5) / float(taps) - 0.5) * dt * 0.5;
            float3 pose = camera_pose(sample_time, 0.0) - correction;
            result += fx_sample_border(input, camera_uv(uv, pose, zoom)) / float(taps);
        }
        output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
        return;
    }
    float2 offset_px = float2(0.0, 0.0);

    if (m == 0)
    {
        offset_px.x = (fx_value_noise1(seed, t * speed) - 0.5) * 2.0 * amount;
        offset_px.y = (fx_value_noise1(seed ^ 0x5555u, t * speed) - 0.5) *
                      2.0 * amount;
    }
    else if (m == 2)
    {
        const uint band = uint(uv.y * 480.0) / 8u;
        const float band_noise =
            fx_value_noise1(seed ^ band * 977u, t * speed * 3.0) - 0.5;
        const float gate =
            fx_hash01(fx_hash4(seed, uint(t * 59.94), band, 7u)) > 0.92 ? 3.0 : 0.35;
        offset_px.x = band_noise * 2.0 * amount * gate;
    }

    const float2 shifted_uv = uv + offset_px / float2(canvas_w, canvas_h);
    const float4 result = fx_sample_border(input, shifted_uv);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
