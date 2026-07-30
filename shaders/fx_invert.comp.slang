// Invert: plain negative, film negative (inversion seen through the C-41
// orange print mask — the strip-of-negatives look), or luma-only
// inversion (hues survive, tones flip). Encoded-domain.
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
    float mode;     // 0 invert, 1 film negative, 2 luma-only
    float amount;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const uint m = uint(mode + 0.5);
    float3 flipped;
    if (m == 1u) {
        // Film negative: invert, then multiply through the orange mask
        // and lift the floor a touch (film base is never black).
        const float3 mask = float3(1.0, 0.62, 0.30);
        flipped = (1.0 - enc) * mask;
        flipped = flipped * 0.92 + 0.05;
    } else if (m == 2u) {
        // Luma-only: flip tone, keep the chroma vector.
        const float l = fx_luma(enc);
        flipped = saturate(enc + ((1.0 - l) - l));
    } else {
        flipped = 1.0 - enc;
    }
    const float3 outc = lerp(enc, flipped, saturate(amount));

    const float4 result = float4(fx_srgb_eotf(saturate(outc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
