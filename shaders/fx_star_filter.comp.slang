// Star / cross-screen filter: the etched-grating lens filter — clipped
// highlights grow diffraction spikes. N evenly spaced spike directions,
// energy gathered inward along each so every bright point streaks.
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
    float points;      // spike count
    float length_px;   // spike reach
    float threshold;   // highlight cut
    float angle;       // filter rotation, degrees
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

    const int n = clamp(int(points + 0.5), 2, 8);
    const float a0 = angle * 0.0174533;
    float3 spike = float3(0.0, 0.0, 0.0);
    for (int k = 0; k < n; ++k) {
        const float a = a0 + float(k) * 6.2831853 / float(n);
        const float2 dir = float2(cos(a), sin(a));
        [unroll]
        for (int s = 1; s <= 16; ++s) {
            const float f = float(s) / 16.0;
            const float d = f * f * length_px;   // taps cluster near core
            const float2 p = clamp(uv + dir * d / size, 0.0, 1.0);
            const float3 c = input.SampleLevel(p, 0).rgb;
            const float3 over = max(c - threshold, 0.0);
            // Slight cool-warm split down the spike sells the grating.
            const float3 disperse =
                lerp(float3(1.0, 0.95, 0.85), float3(0.85, 0.9, 1.0), f);
            spike += over * disperse * (1.0 - f) * (1.0 - f);
        }
    }
    spike *= 1.6 / (16.0 * float(n));

    const float4 result = float4(base.rgb + spike, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
