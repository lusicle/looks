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
    float amount;
    float zoom;        // per pass
    float rotate;      // radians per pass
    float fade;
    float hue_shift;   // degrees per pass
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev_out;
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

    // Inverse map. Sample where this pixel was in the last frame.
    const float aspect = size.x / size.y;
    float2 p = uv - 0.5;
    p.x *= aspect;
    const float dt = 60.0 / max(fps, 1.0);
    const float retention = pow(saturate(amount) * (1.0 - saturate(fade)), dt);
    const float inv_zoom = pow(max(zoom, 0.05), -dt);
    const float c = cos(-rotate * dt);
    const float s = sin(-rotate * dt);
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c) * inv_zoom;
    p.x /= aspect;
    const float2 prev_uv = p + 0.5;

    float3 fb = float3(0.0, 0.0, 0.0);
    float fb_a = 0.0;
    if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
        const float4 prev = prev_out.SampleLevel(prev_uv, 0);
        fb = prev.rgb * retention;
        fb_a = prev.a * retention;
    }

    if (abs(hue_shift) > 0.01) {
        const float ha = hue_shift * 0.01745329252 * dt;
        const float hc = cos(ha), hs = sin(ha);
        const float3 k = float3(0.57735, 0.57735, 0.57735);
        fb = fb * hc + cross(k, fb) * hs + k * dot(k, fb) * (1.0 - hc);
        fb = max(fb, 0.0);
    }

    const float3 mixed = max(base.rgb, fb);
    const float4 result =
        float4(mixed, max(base.a, fb_a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
