// Polar coordinates: rect→polar wraps the frame around the
// center (x = angle, y = radius); polar→rect unwraps it into a strip.
// swirl shears the angle by radius; the angle axis wraps, radius clamps.
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
    float mode;    // 0 = rect source read in polar, 1 = unwrap to a strip
    float swirl;   // radians of angular shear across the radius
    float zoom;    // radial scale
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

    float2 suv;
    if (mode < 0.5) {
        // Output pixel (x=angle, y=radius) reads the rect source in polar.
        const float theta = uv.x * 2.0 * kPi + swirl * uv.y;
        const float r = uv.y * 0.5 * max(zoom, 1.0e-3);
        suv = float2(0.5, 0.5) +
              r * float2(cos(theta), sin(theta)) *
                  float2(size.y / size.x, 1.0);   // circular on screen
        suv = clamp(suv, 0.0, 1.0);
    } else {
        // Unwrap: output row = radius ring, column = angle.
        const float2 d = (uv - 0.5) * float2(size.x / size.y, 1.0);
        const float r = length(d) / (0.5 * max(zoom, 1.0e-3));
        const float theta = atan2(d.y, d.x) - swirl * r;
        float ax = theta / (2.0 * kPi);
        ax -= floor(ax);                    // wrap the angle axis
        suv = float2(ax, clamp(r, 0.0, 1.0));
    }
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
