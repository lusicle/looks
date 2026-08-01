// Digital-zoom crunch: crop-zoom + cheap resample — the
// camcorder digital zoom. Crunch snaps sampling onto the pre-zoom pixel
// grid (fat aliased blocks) and adds the oversharpen ring cameras used to
// hide the mush.
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
    float zoom;      // crop factor about center (1 = none)
    float crunch;    // 0 smooth resample, 1 fully blocky + ringed
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float z = max(zoom, 1.0);
    float2 suv = (uv - 0.5) / z + 0.5;

    // Crunch: quantize onto the source pixel grid (nearest-neighbor
    // blocks that grow with the zoom).
    const float2 snapped = (floor(suv * size) + 0.5) / size;
    suv = lerp(suv, snapped, saturate(crunch));
    float3 col = input.SampleLevel(clamp(suv, 0.0, 1.0), 0).rgb;

    // Camera-style oversharpen ring on the crunched image.
    if (crunch > 0.001) {
        const float2 e = z / size;   // one source pixel at this zoom
        float3 ring = 0.0;
        ring += input.SampleLevel(clamp(suv + float2(e.x, 0.0), 0.0, 1.0), 0).rgb;
        ring += input.SampleLevel(clamp(suv - float2(e.x, 0.0), 0.0, 1.0), 0).rgb;
        ring += input.SampleLevel(clamp(suv + float2(0.0, e.y), 0.0, 1.0), 0).rgb;
        ring += input.SampleLevel(clamp(suv - float2(0.0, e.y), 0.0, 1.0), 0).rgb;
        col = max(col + (col - ring * 0.25) * crunch * 1.2, 0.0);
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
