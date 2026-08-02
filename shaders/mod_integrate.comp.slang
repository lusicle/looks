// One thread per lane, marching across the frame accumulating FM phase
// per channel (R, G, B) plus a luma phase in .a. Frequency is clamped
// positive: phase must stay monotonic for the crest search downstream.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float omega;
    float distortion;
    float lowpass;
    uint mode;          // bit 0: row lanes; bit 1: reversed march. The
                        // march always follows the travel direction, so
                        // the causal lowpass wake sits behind the lines
                        // rather than on the incoming side.
    float knee;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> integral;

[shader("compute")]
[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const bool vert = (mode & 1u) != 0u;
    const bool rev = (mode & 2u) != 0u;
    const uint lanes = vert ? height : width;
    if (tid.x >= lanes)
        return;
    const uint steps = vert ? width : height;
    const float2 size = float2(width, height);

    const float alpha = lerp(1.0, 0.03, saturate(lowpass));

    float4 sig_s = float4(-1.0, -1.0, -1.0, -1.0);   // filter state
    float4 phi = float4(0.0, 0.0, 0.0, 0.0);

    // The lateral 3-tap average below is load-bearing: neighbouring lanes
    // must accumulate similar phase through detailed regions, or the
    // traces downstream shatter into incoherent fragments.
    const float2 d_lane =
        (vert ? float2(0.0, 1.0) : float2(1.0, 0.0)) / size;

    for (uint s = 0; s < steps; ++s) {
        const uint pos = rev ? steps - 1u - s : s;
        const uint2 px = vert ? uint2(pos, tid.x) : uint2(tid.x, pos);
        const float2 uv = (float2(px) + 0.5) / size;
        const float3 rgb = fx_srgb_oetf(saturate(
            input.SampleLevel(uv, 0).rgb * 0.5 +
            input.SampleLevel(clamp(uv - d_lane, 0.0, 1.0), 0).rgb *
                0.25 +
            input.SampleLevel(clamp(uv + d_lane, 0.0, 1.0), 0).rgb *
                0.25));
        const float4 sig = float4(rgb, fx_luma(rgb));

        if (sig_s.x < 0.0) sig_s = sig;               // filter warm-up
        sig_s += alpha * (sig - sig_s);

        // Negative distortion inverts the signal.
        const float4 s4 =
            distortion < 0.0
                ? float4(1.0, 1.0, 1.0, 1.0) - sig_s
                : sig_s;
        // distortion is the EXTRA multiplier at full density (0 = off),
        // and the response stays continuous everywhere: no dead zone, so
        // even subtle shading slows the lines a little.
        const float dext = abs(distortion);
        const float4 w4 = pow(max(s4, float4(0.0, 0.0, 0.0, 0.0)),
                              float4(1.0, 1.0, 1.0, 1.0) *
                                  (1.0 + knee * 3.0));
        // Base capped at half the density cap so detail modulation keeps
        // 2x headroom at any line count; total capped at 0.25 cycles/px,
        // a 4px minimum trace pitch.
        const float base_f = min(omega / float(steps), 0.125);
        const float4 f = min(base_f * (1.0 + dext * w4),
                             float4(0.25, 0.25, 0.25, 0.25));
        phi += f;
        integral[px] = phi;
    }
}
