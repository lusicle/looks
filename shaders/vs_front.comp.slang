// Texel x is the along-axis position and y is the line slot.
// State is R = front px, G = active, B = spawn tick + 1, 0 = never.

#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint state_w;   // along-axis texels, the max frame dimension
    uint slots;
    uint seed;
    uint frame;
    float fps;
    float speed;    // px/s
    float stick;
    float rate;     // spawns/s, staggered across slots
    float wiggle;
    uint mode;
    uint frame_w;
    uint frame_h;
    float canvas_w;
    float canvas_h;
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
    const float extent = mode == 0u ? canvas_w : canvas_h;
    if (tid.x >= along_max) {
        front_new[tid.xy] = st;
        return;
    }

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = uint(t * max(rate, 0.01));

    if (st.g < 0.5) {
        if (tick % slots == tid.y && st.b != float(tick) + 1.0) {
            st = float4(0.0, 1.0, float(tick) + 1.0, 0.0);
        }
    } else {
        if (st.r >= extent) {
            st.g = 0.0;
            front_new[tid.xy] = st;
            return;
        }
        const float2 uv = mode == 0u
            ? float2(st.r / canvas_w,
                     (float(tid.x) + 0.5) / float(frame_h))
            : float2((float(tid.x) + 0.5) / float(frame_w),
                     st.r / canvas_h);
        const float l = fx_enc_luma(video, saturate(uv));
        const float brake =
            lerp(1.0, pow(saturate(1.0 - l), 3.0), saturate(stick));
        const float shear =
            1.0 + (fx_value_noise1(seed ^ (tid.y + 1u) * 131u,
                                   float(tid.x) / float(along_max) *
                                   (mode == 0u ? canvas_h : canvas_w) * 0.03 + t * 1.7) - 0.5) *
                      wiggle * 1.6;
        st.a = st.r;
        st.r += speed / max(fps, 1.0) * brake * shear;
        // A stuck front retires after one full spawn cycle.
        if (float(tick) >= st.b - 1.0 + float(slots)) st.g = 0.0;
    }
    front_new[tid.xy] = st;
}
