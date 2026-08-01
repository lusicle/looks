// Velocity-scan front stepper: each texel is one segment of one sweeping
// line — x = along-axis position, y = line slot. The segment's front
// advances across the frame at a speed braked by the luma underneath
// (velocity modulation: brightness = dwell time, the 1930s TV scheme).
// Slots respawn from the frame edge on a staggered tick cadence and die
// after one full cycle so stuck fronts always recycle. Deterministic:
// frame-clocked, seeded.
//
// State texel: R = front position (px along the sweep axis), G = active,
// B = spawn tick + 1 (0 = never spawned).

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint state_w;   // along-axis texels (max frame dimension)
    uint slots;     // concurrent sweep lines
    uint seed;
    uint frame;
    float fps;
    float speed;    // sweep px/s
    float stick;    // luma braking strength
    float rate;     // spawns per second (staggered across slots)
    float wiggle;   // shear noise on the advance
    uint mode;      // 0 = vertical lines sweep +x, 1 = horizontal sweep +y
    uint frame_w;
    uint frame_h;
};

[[vk::binding(0, 0)]] Sampler2D front_old;
[[vk::binding(1, 0)]] Sampler2D video;
[[vk::binding(2, 0)]] RWTexture2D<float4> front_new;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= state_w || tid.y >= slots)
        return;
    const float2 st_uv = (float2(tid.xy) + 0.5) /
                         float2(float(state_w), float(slots));
    float4 st = front_old.SampleLevel(st_uv, 0);

    const uint along_max = mode == 0u ? frame_h : frame_w;
    const float extent = mode == 0u ? float(frame_w) : float(frame_h);
    if (tid.x >= along_max) {
        front_new[tid.xy] = st;
        return;
    }

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = uint(t * max(rate, 0.01));

    if (st.g < 0.5) {
        // Staggered respawn: this slot's turn in the tick cycle, once.
        if (tick % slots == tid.y && st.b != float(tick) + 1.0) {
            st = float4(0.0, 1.0, float(tick) + 1.0, 0.0);
        }
    } else {
        // Luma under the front brakes it — brightness becomes dwell.
        const float2 uv = mode == 0u
            ? float2((st.r + 0.5) / float(frame_w),
                     (float(tid.x) + 0.5) / float(frame_h))
            : float2((float(tid.x) + 0.5) / float(frame_w),
                     (st.r + 0.5) / float(frame_h));
        const float l = fx_luma(fx_srgb_oetf(
            saturate(video.SampleLevel(saturate(uv), 0).rgb)));
        const float brake =
            lerp(1.0, pow(saturate(1.0 - l), 3.0), saturate(stick));
        // Shear: neighbors advance at slightly different rates, so the
        // trace wavers like a hand-deflected beam.
        const float shear =
            1.0 + (fx_value_noise1(seed ^ (tid.y + 1u) * 131u,
                                   float(tid.x) * 0.03 + t * 1.7) - 0.5) *
                      wiggle * 1.6;
        st.r += max(speed / max(fps, 1.0) * brake * shear, 0.04);
        // Retire past the far edge, or after one full spawn cycle so a
        // fully stuck front still frees its slot.
        if (st.r >= extent) st.g = 0.0;
        if (float(tick) >= st.b - 1.0 + float(slots)) st.g = 0.0;
    }
    front_new[tid.xy] = st;
}
