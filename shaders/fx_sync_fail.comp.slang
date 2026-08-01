// Sync failure: vertical roll (the picture wraps around at a
// steady rate) and a horizontal tear band that wanders the frame shearing
// scanlines sideways. All motion is timeline-clocked — deterministic.
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
    float roll;    // vertical roll speed, hz (frames of the picture per s)
    float tear;    // horizontal tear strength
    float band;    // tear band height (normalized)
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;

    // Roll: wrap vertically; a thin dark blanking bar marks the wrap seam.
    float sy = uv.y + t * roll;
    sy -= floor(sy);

    // Tear: the band center wanders; inside it, scanlines shear by a
    // per-line jitter on top of a base displacement.
    const float center = fx_value_noise1(seed, t * 1.7);
    const float d = abs(sy - center);
    float sx = uv.x;
    if (d < band) {
        const float fall = 1.0 - d / band;
        const float line_jit =
            fx_hash01(fx_hash4(seed, frame, tid.y, 7u)) - 0.5;
        sx += (0.25 + 0.15 * line_jit) * tear * fall;
        sx -= floor(sx);
    }

    float3 rgb = input.SampleLevel(float2(sx, sy), 0).rgb;
    // Blanking bar at the wrap seam.
    const float seam = min(sy, 1.0 - sy);
    if (roll > 0.0 && seam < 0.012) rgb *= 0.25;

    const float4 result = float4(rgb, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
