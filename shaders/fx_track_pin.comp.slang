#include "fx_common.slang"

// The engine sends one row-major 3x3 that maps output uv to sample uv.

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
    float region_x;
    float region_y;
    float region_w;
    float region_h;
    float offset_x;
    float offset_y;
    float pin_scale;
    float angle;
    float m00; float m01; float m02;
    float m10; float m11; float m12;
    float m20; float m21; float m22;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D pin_b;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float w = m20 * uv.x + m21 * uv.y + m22;
    const float iw = abs(w) > 1.0e-12 ? 1.0 / w : 0.0;
    const float2 suv = float2(m00 * uv.x + m01 * uv.y + m02,
                              m10 * uv.x + m11 * uv.y + m12) * iw;

    float4 result;
    if (mode < 0.5)
    {
        float4 b = float4(0.0, 0.0, 0.0, 0.0);
        if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0)
            b = pin_b.SampleLevel(suv, 0);
        result = b + base * (1.0 - b.a);
    }
    else
    {
        // Off-frame reads transparent black, the closed-gate convention.
        const bool inside = suv.x >= 0.0 && suv.x <= 1.0 &&
                            suv.y >= 0.0 && suv.y <= 1.0;
        result = inside ? input.SampleLevel(suv, 0)
                        : float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
