// Projector cue marks (spec §6.8): the reel-change dot — a rough circle in
// the top-right corner that appears for a few frames every period. The
// second (changeover) mark follows one second after the first, like the
// real thing. Encoded space, slightly ragged edge.
//
// Composition: final = mix(in, blend(in, mix(in, fx(in), wet)), opacity).

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
    float period;      // seconds between cue events
    float dwell;       // frames the mark stays on
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
    const uint period_frames = max(uint(period * fp), 2u);
    const uint in_period = frame % period_frames;
    const uint dwell_f = max(uint(dwell), 1u);
    // Motor cue at the period start, changeover cue one second later.
    const bool on = in_period < dwell_f ||
                    (in_period >= uint(fp) && in_period < uint(fp) + dwell_f);

    float mark = 0.0;
    if (on) {
        const float2 center = float2(0.93, 0.09);
        const float2 d = (uv - center) * float2(size.x / size.y, 1.0);
        const float rr = length(d) / max(mark_size, 1.0e-3);
        // Ragged punched edge: radius wobbles per angle, re-seeded per event.
        const uint event = frame / period_frames;
        const float ang = atan2(d.y, d.x);
        const float wobble =
            0.85 + 0.25 * fx_value_noise1(seed ^ event, ang * 2.2 + 10.0);
        mark = rr < wobble ? 1.0 : 0.0;
    }

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    enc = lerp(enc, float3(0.10, 0.08, 0.07), mark * 0.9);   // punched dark

    const float4 result = float4(fx_srgb_eotf(enc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
