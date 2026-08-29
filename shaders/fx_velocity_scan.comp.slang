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
    float mode;
    float speed;     // the stepper uses this, not the shader
    float stick;     // the stepper uses this, not the shader
    float persist;
    float rate;      // the stepper uses this, not the shader
    float beam_w;    // half-width, px
    float wiggle;    // the stepper uses this, not the shader
    float color;
    uint state_w;    // the engine appends the front field width
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;
[[vk::binding(2, 0)]] Sampler2D front;
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

    const float4 held = canvas.SampleLevel(uv, 0);
    float3 col = held.rgb * saturate(persist);
    float col_a = held.a * saturate(persist);

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
            col_a = max(col_a, glow * glow * 0.35);
        }
    }
    // Use a ceiling instead of blowout so bunched fronts stay readable.
    col = min(col, float3(1.6, 1.6, 1.6));

    const float4 result = float4(col, saturate(col_a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
