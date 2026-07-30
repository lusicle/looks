// Lens flare: screen-space ghost train — clipped highlights re-image as a
// row of aperture ghosts marching through the frame center, plus a halo
// ring at fixed radius. Ghost samples come from center-mirrored, scaled
// copies of the thresholded frame (the classic pseudo-flare construction).
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
    float threshold;
    float ghosts;
    float spread;
    float halo;
    float tint;     // chromatic spread along the ghost train
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float3 bright_at(float2 p, float t)
{
    const float3 c = input.SampleLevel(clamp(p, 0.0, 1.0), 0).rgb;
    return max(c - t, 0.0);
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

    const int n = clamp(int(ghosts + 0.5), 1, 6);
    const float2 to_center = float2(0.5, 0.5) - uv;
    float3 flare = float3(0.0, 0.0, 0.0);
    for (int i = 1; i <= n; ++i) {
        // Ghost i lives at the source position mirrored/scaled through
        // center: sample the thresholded frame there and let it fade and
        // rainbow-shift down the train.
        const float s = spread * float(i) * 0.6;
        const float2 gp = uv + to_center * (1.0 + s);
        const float fade = 1.0 / (1.0 + float(i) * float(i) * 0.6);
        const float3 disperse = lerp(
            float3(1.0, 1.0, 1.0),
            fx_hue_to_rgb(frac(float(i) * 0.17 + 0.55)) * 1.4,
            saturate(tint));
        // A soft small blur per ghost: 4 taps.
        float3 g = float3(0.0, 0.0, 0.0);
        [unroll]
        for (int k = 0; k < 4; ++k) {
            const float a = float(k) * 1.5708 + float(i);
            g += bright_at(gp + float2(cos(a), sin(a)) * 4.0 / size,
                           threshold);
        }
        flare += g * 0.25 * fade * disperse;
    }
    // Halo: the thresholded frame sampled on a ring around center.
    if (halo > 0.001) {
        const float rlen = length(to_center * float2(size.x / size.y, 1.0));
        const float ring =
            exp(-abs(rlen - 0.28) * 22.0);
        if (ring > 0.01) {
            const float2 hp = uv + normalize(to_center + 1.0e-5) *
                                       0.35;
            flare += bright_at(hp, threshold) * ring * halo *
                     float3(0.6, 0.75, 1.1);
        }
    }

    const float4 result = float4(base.rgb + flare * 1.4, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
