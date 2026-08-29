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
    float amount;
    float radius;     // relative to half the short side
    float center_x;   // normalized
    float center_y;
    float shade;
    float backdrop;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float kPi = 3.14159265358979;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float2 center = float2(center_x, center_y);
    const float half_min = 0.5 * min(size.x, size.y) * max(radius, 1.0e-3);
    // r is 1 on the rim. This keeps the ball circular at any aspect.
    const float2 d = (uv - center) * size / half_min;
    const float r = length(d);

    float4 result;
    if (r <= 1.0) {
        const float ball_r = asin(saturate(r)) * (2.0 / kPi);
        const float src_r = lerp(r, ball_r, saturate(amount));
        const float2 dir = r > 1.0e-5 ? d / r : float2(0.0, 0.0);
        const float2 suv = center + dir * src_r * half_min / size;
        result = input.SampleLevel(clamp(suv, 0.0, 1.0), 0);
        const float cosine = sqrt(saturate(1.0 - r * r));
        result.rgb *= lerp(1.0, cosine, saturate(shade) * saturate(amount));
    } else {
        // The backdrop keeps the layer coverage. Never add opacity here.
        result = backdrop >= 0.5 ? base : float4(0.0, 0.0, 0.0, base.a);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
