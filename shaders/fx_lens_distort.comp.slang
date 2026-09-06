// The aspect correction keeps the distortion circular on screen.

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
    float k1;      // r^2 coefficient, positive barrels
    float k2;      // r^4 coefficient
    float zoom;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float2 lens_map(float2 uv, float aspect)
{
    float2 d = (uv - 0.5) * float2(aspect, 1.0) * 2.0;
    float r2 = dot(d, d);
    float limit = aspect * aspect + 1.0;
    float minimum = min(1.0, 1.0 + 3.0 * k1 * limit + 5.0 * k2 * limit * limit);
    if (k2 > 0.0) {
        float stationary = clamp(-3.0 * k1 / (10.0 * k2), 0.0, limit);
        minimum = min(minimum, 1.0 + 3.0 * k1 * stationary + 5.0 * k2 * stationary * stationary);
    }
    float strength = minimum < 0.05 ? 0.95 / (1.0 - minimum) : 1.0;
    float f = 1.0 + strength * (k1 * r2 + k2 * r2 * r2);
    return d * f / max(zoom, 0.001) * 0.5 / float2(aspect, 1.0) + 0.5;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);
    if (k1 == 0.0 && k2 == 0.0 && zoom == 1.0) {
        output[tid.xy] = base;
        return;
    }

    const float aspect = size.x / size.y;
    float4 result = 0.0;
    for (int y = -1; y <= 1; y += 2)
        for (int x = -1; x <= 1; x += 2)
            result += fx_sample_border(input, lens_map(uv + float2(x, y) * 0.25 / size, aspect));
    result *= 0.25;

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
