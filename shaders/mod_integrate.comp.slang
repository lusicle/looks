// Modulation phase integrator (GenerateMe fm.pde model): one thread per
// lane, marching across the frame accumulating the FM phase —
//   phi += (omega + distortion * signal) / extent
// per channel (R, G, B) plus a luma phase in .a. The signal runs through
// a 1-pole IIR lowpass first (the sketch's LowpassFilter), so the
// instantaneous frequency — and with it the trace spacing — changes
// smoothly. Frequency is clamped positive: phase must stay monotonic.
//
// mode 0: horizontal traces — lanes are columns, march down.
// mode 1: vertical traces — lanes are rows, march right.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float omega;        // base cycles across the frame
    float distortion;   // frequency multiplier at full signal
    float lowpass;      // 0 raw .. 1 heavy smoothing
    uint mode;          // bit 0: 1 = row lanes (vertical traces);
                        // bit 1: 1 = reversed march (travel flipped).
                        // The march ALWAYS follows the travel direction —
                        // lines spawn at the edge they enter from, so the
                        // causal lowpass wake sits behind them, never on
                        // the incoming side.
    float knee;         // response curve: 0 linear, higher = brights-weighted
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

    // 1-pole IIR: alpha 1 = no filtering, small = heavy smoothing.
    const float alpha = lerp(1.0, 0.03, saturate(lowpass));

    float4 sig_s = float4(-1.0, -1.0, -1.0, -1.0);   // filter state
    float4 phi = float4(0.0, 0.0, 0.0, 0.0);

    // Lateral 3-tap average: neighbouring lanes must accumulate similar
    // phase through detailed regions, or the traces downstream shatter
    // into incoherent fragments.
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

        // Traveling-line model: line speed is LOCAL. Below the knee the
        // lines cruise at uniform speed (uniform spacing — approach
        // gradients must NOT pre-slow them); crossing the knee the
        // frequency multiplies up to `distortion` x and the lines pile
        // AT the region. Negative distortion inverts the signal.
        const float4 s4 =
            distortion < 0.0
                ? float4(1.0, 1.0, 1.0, 1.0) - sig_s
                : sig_s;
        // `distortion` is the EXTRA frequency multiplier at full
        // density: 0 = off, 0.5 = 1.5x, continuous from zero.
        // Response is continuous everywhere — even subtle dark shading
        // slows the lines a little (no dead zone). `knee` curves the
        // response: 0 = linear, higher bends it toward the brights
        // while the darks keep a gentle, proportional effect.
        const float dext = abs(distortion);
        const float4 w4 = pow(max(s4, float4(0.0, 0.0, 0.0, 0.0)),
                              float4(1.0, 1.0, 1.0, 1.0) *
                                  (1.0 + knee * 3.0));
        // Base capped at half the density cap so detail modulation
        // always keeps at least 2x headroom no matter the line count;
        // total capped at 0.25 cycles/px (4px minimum trace pitch).
        const float base_f = min(omega / float(steps), 0.125);
        const float4 f = min(base_f * (1.0 + dext * w4),
                             float4(0.25, 0.25, 0.25, 0.25));
        phi += f;
        integral[px] = phi;
    }
}
