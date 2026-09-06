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
    float canvas_w;
    float canvas_h;
    uint history_valid;
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

float ramp_area(float x, float span)
{
    float t = clamp(x, 0.0, span);
    return t * (x - 0.5 * t);
}

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
    const float sweep_p = rows ? uv.y * canvas_h : uv.x * canvas_w;
    const float pixel = rows ? canvas_h / float(height) : canvas_w / float(width);

    const float4 held = canvas.SampleLevel(uv, 0);
    const float dt = 60.0 / max(fps, 1.0);
    const float retention = pow(saturate(persist), dt);
    float3 col = held.rgb * retention;
    float col_a = held.a * retention;

    const float3 src_col =
        lerp(float3(1.0, 1.0, 1.0), base.rgb * 1.4, saturate(color));
    for (uint s = 0u; s < kSlots; ++s) {
        const float2 st_uv =
            float2((float(along) + 0.5) / float(state_w),
                   (float(s) + 0.5) / float(kSlots));
        const float4 st = front.SampleLevel(st_uv, 0);
        if (st.g < 0.5) continue;
        const float travel = st.r - st.a;
        const float x = sweep_p - st.a;
        if (x < -beam_w - pixel * 0.5 || x > travel + beam_w + pixel * 0.5) continue;
        float coverage;
        if (travel > 1.0e-4) {
            coverage = (ramp_area(x + beam_w + pixel * 0.5, travel) -
                        ramp_area(x - beam_w + pixel * 0.5, travel) -
                        ramp_area(x + beam_w - pixel * 0.5, travel) +
                        ramp_area(x - beam_w - pixel * 0.5, travel)) / (travel * pixel);
        } else {
            coverage = max(min(x + pixel * 0.5, beam_w) -
                           max(x - pixel * 0.5, -beam_w), 0.0) / pixel;
        }
        if (coverage > 0.0) {
            const float energy = saturate(coverage) * 0.35 * dt;
            col += src_col * energy;
            col_a = max(col_a, energy);
        }
    }
    // Use a ceiling instead of blowout so bunched fronts stay readable.
    col = min(col, float3(1.6, 1.6, 1.6));

    const float4 result = float4(col, saturate(col_a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
