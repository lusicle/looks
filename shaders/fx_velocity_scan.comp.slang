// Velocity Scan (dwell-time rendering): draws the sweeping line fronts
// over a decaying phosphor canvas — the effect's own previous output.
// Bright regions accumulate stuck beam segments from every sweep; dark
// regions flash only as a line hurries through. Brightness is rendered
// as time-spent, not intensity — velocity-modulated scanning.
//
// Inputs: 0 = frame, 1 = canvas (own previous output), 2 = front field.
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
    float mode;      // 0 = vertical lines sweep +x, 1 = horizontal +y
    float speed;     // (stepper param; unused here)
    float stick;     // (stepper param; unused here)
    float persist;   // canvas decay per frame
    float rate;      // (stepper param; unused here)
    float beam_w;    // beam half-width, px
    float wiggle;    // (stepper param; unused here)
    float color;     // 0 = white beam, 1 = source-colored beam
    uint state_w;    // engine-appended: front-field width
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;   // own previous output
[[vk::binding(2, 0)]] Sampler2D front;    // sweep-front field
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

static const uint kSlots = 24;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) /
                      float2(float(width), float(height));
    const float4 base = input.SampleLevel(uv, 0);

    const bool rows = mode >= 0.5;
    const uint along = rows ? tid.x : tid.y;
    const float sweep_p = rows ? float(tid.y) + 0.5 : float(tid.x) + 0.5;

    // Phosphor: last frame's glow, decayed.
    float3 col = canvas.SampleLevel(uv, 0).rgb * saturate(persist);

    // Splat every active front passing this pixel. The beam runs at
    // constant intensity — the LOOK comes from dwell accumulation.
    const float3 src_col =
        lerp(float3(1.0, 1.0, 1.0), base.rgb * 1.4, saturate(color));
    for (uint s = 0u; s < kSlots; ++s) {
        const float2 st_uv =
            float2((float(along) + 0.5) / float(state_w),
                   (float(s) + 0.5) / float(kSlots));
        const float4 st = front.SampleLevel(st_uv, 0);
        if (st.g < 0.5) continue;
        const float d = abs(sweep_p - st.r);
        if (d < beam_w) {
            const float glow = 1.0 - d / beam_w;
            col += src_col * glow * glow * 0.35;
        }
    }
    // Phosphor saturates — dwell piles up to a ceiling instead of HDR
    // blowout, which keeps bunched-up stuck fronts reading as material.
    col = min(col, float3(1.6, 1.6, 1.6));

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
