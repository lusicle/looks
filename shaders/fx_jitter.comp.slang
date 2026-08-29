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
    float amount;      // px of travel
    float speed;       // hz
    float mode;
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const int m = int(mode + 0.5);
    float2 offset_px = float2(0.0, 0.0);

    if (m == 0)
    {
        offset_px.x = (fx_value_noise1(seed, t * speed) - 0.5) * 2.0 * amount;
        offset_px.y = (fx_value_noise1(seed ^ 0x5555u, t * speed) - 0.5) *
                      2.0 * amount;
    }
    else if (m == 1)
    {
        const float phase = t * speed * 2.0;
        const uint step_i = uint(int(floor(phase)) + 1000000);
        const float held_x =
            (fx_hash01(fx_hash3(seed, step_i, 1u)) - 0.5) * 2.0;
        const float held_y =
            (fx_hash01(fx_hash3(seed, step_i, 2u)) - 0.5) * 2.0;
        const float f = frac(phase);
        const float settle = 1.0 - smoothstep(0.0, 0.2, f) * 0.85;
        offset_px = float2(held_x, held_y) * amount * settle;
    }
    else if (m == 2)
    {
        const uint band = tid.y / 8;
        const float band_noise =
            fx_value_noise1(seed ^ band * 977u, t * speed * 3.0) - 0.5;
        const float gate =
            fx_hash01(fx_hash4(seed, frame, band, 7u)) > 0.92 ? 3.0 : 0.35;
        offset_px.x = band_noise * 2.0 * amount * gate;
    }
    else
    {
        const float row_delay = uv.y * 0.6;
        const float sway =
            fx_value_noise1(seed, (t - row_delay / max(speed, 0.1)) * speed) -
            0.5;
        offset_px.x = sway * 2.0 * amount;
        offset_px.y = (fx_value_noise1(seed ^ 0x9999u,
                                       t * speed * 0.7 + uv.y * 2.0) -
                       0.5) * amount * 0.3;
    }

    const float2 shifted_uv = uv + offset_px / size;
    const float4 result = input.SampleLevel(saturate(shifted_uv), 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
