// Lost vertical hold: the picture rolls through a real VBI bar - a fat
// near-black band with sync serration strips - the lines just past it
// flag sideways while the oscillator relocks, and the AGC dips while
// sync is unstable. The tear band is a separate horizontal-hold slip.

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
    float roll;    // picture heights per second
    float tear;
    float band;
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

    float sy = uv.y + t * roll;
    sy -= floor(sy);

    // Distance below the wrap seam, in picture fractions (the seam is
    // where vsync happens; relock artifacts hang off its lower edge).
    const float below = frac(1.0 - sy);
    float sx = uv.x;

    // Flagging: the horizontal oscillator relocks over the first lines
    // after vsync, bending them sideways.
    if (roll > 0.0) {
        const float flag_w =
            (fx_value_noise1(seed ^ 0x8F1u, t * 2.4) - 0.5) * 2.0;
        sx += flag_w * tear * 0.08 * exp(-below * 26.0);
    }

    // Horizontal-hold slip: a wandering band tears sideways.
    const float center = fx_value_noise1(seed, t * 1.7);
    const float d = abs(sy - center);
    if (d < band) {
        const float fall = 1.0 - d / band;
        const float line_jit =
            fx_hash01(fx_hash4(seed, frame, tid.y, 7u)) - 0.5;
        sx += (0.25 + 0.15 * line_jit) * tear * fall;
    }
    sx -= floor(sx);

    const float4 rolled = input.SampleLevel(float2(sx, sy), 0);
    float3 rgb = rolled.rgb;
    float out_a = rolled.a;

    if (roll > 0.0) {
        // The VBI bar: ~6% of the picture, near-black, with the sync
        // serration strips reading as dashed gray lines inside it.
        const float seam = min(sy, 1.0 - sy);
        const float half_h = 0.031;
        const float bar =
            1.0 - smoothstep(half_h * 0.8, half_h, seam);
        if (bar > 0.0) {
            const float pos = seam / half_h;   // 0 seam .. 1 bar edge
            float lum = 0.012;
            // Serrations at half-line rate: six dashes across.
            const float dash =
                step(0.5, frac(uv.x * 6.0)) * 0.14;
            if (pos > 0.30 && pos < 0.44) lum += dash;
            if (pos > 0.62 && pos < 0.76) lum += dash;
            rgb = lerp(rgb, float3(lum, lum, lum), bar);
            // The bar is signal structure, not transparency.
            out_a = max(out_a, bar);
        }
        // AGC hunts while sync is unstable.
        rgb *= 1.0 - 0.12 * saturate(roll * 2.0);
    }

    const float4 result = float4(rgb, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
