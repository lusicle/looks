// Folds per channel rather than on luma, which is what shears color into
// the metallic look.

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
    float folds;
    float knee;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float fold1(float x, float t, float k)
{
    // Reflect x about t, then renormalize so the full input range keeps
    // using the full output range.
    const float over = x - t;
    if (k > 1.0e-3) {
        // sqrt(over^2 + k^2) - k is a smooth |over|, rounding the crease.
        const float folded = t - (sqrt(over * over + k * k) - k);
        return max(folded, 0.0) / max(t, 1.0e-3) * t;
    }
    return x <= t ? x : max(2.0 * t - x, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const int n = clamp(int(folds + 0.5), 1, 4);
    const float t = clamp(threshold, 0.05, 0.95);
    const float k = knee * 0.12;
    for (int i = 0; i < n; ++i) {
        enc.x = fold1(enc.x, t, k);
        enc.y = fold1(enc.y, t, k);
        enc.z = fold1(enc.z, t, k);
        // Each extra fold re-expands so the next pass has range to bite.
        if (i + 1 < n) enc = saturate(enc / max(t, 1.0e-3));
    }

    const float4 result = float4(fx_srgb_eotf(saturate(enc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
