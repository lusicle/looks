// Binding 1 is this instance's output from the previous frame.

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
    float period;    // seconds per full frame
    float mode;
    float beam;
    float noise;
    float dim;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;
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

    const bool cols = mode >= 0.5;
    const float per = max(period, 0.25);
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float fstep = fps > 0.0 ? (1.0 / fps) / per : 0.04;
    const float s_now = frac(t / per);
    const float s_prev = s_now - fstep;
    const uint cycle = uint(t / per);

    const float pos = cols ? uv.x : uv.y;
    const uint line_i = cols ? tid.x : tid.y;
    const float extent = cols ? size.x : size.y;

    const bool in_band =
        s_prev >= 0.0 ? (pos > s_prev && pos <= s_now)
                      : (pos <= s_now || pos > s_prev + 1.0);

    float3 col;
    float col_a;
    if (in_band) {
        const float j =
            (fx_hash01(fx_hash4(seed, line_i, cycle, 0x5Cu)) - 0.5) *
            saturate(noise) * 16.0;
        float2 suv = uv;
        if (cols) suv.y = clamp(uv.y + j / size.y, 0.0, 1.0);
        else      suv.x = clamp(uv.x + j / size.x, 0.0, 1.0);
        const float4 s = input.SampleLevel(suv, 0);
        col = s.rgb;
        col_a = s.a;
        const float fz =
            fx_hash01(fx_hash4(seed ^ 0xF1u, tid.x, tid.y, cycle)) - 0.5;
        col = max(col + fz * saturate(noise) * 0.25, 0.0);
    } else {
        const float4 held = canvas.SampleLevel(uv, 0);
        col = held.rgb;
        col_a = held.a;
        if (pos > s_now)
            col *= 1.0 - saturate(dim) * 0.6;
    }

    // Glow only ahead of the write line so it does not burn in.
    const float ahead = (pos - s_now) * extent;
    if (ahead > 0.0) {
        const float glow =
            saturate(beam) * 0.9 * exp(-ahead * ahead * 0.08);
        col += glow * float3(1.0, 1.0, 0.95);
        col_a = max(col_a, glow);
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
