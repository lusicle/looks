// Static per seed: this is a plate, not a video artifact, so nothing
// here reads the frame clock. Display-referred, self-converting.

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
    float ortho;
    float tone;     // 0 cold silver, 1 warm sepia
    float plate;
    float fog;
    float vign;
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

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    // Blue-sensitive collodion response; weights sum to 1 at both ends.
    const float3 w = lerp(float3(0.2126, 0.7152, 0.0722),
                          float3(0.05, 0.25, 0.70), saturate(ortho));
    float l = dot(enc, w);

    l = pow(l, 0.9);
    l = l * 0.92 + 0.04 + saturate(fog) * 0.2 * (1.0 - l);

    // Pour marks: swirl noise gated to the border zone.
    const float2 border = min(uv, 1.0 - uv);
    const float edge_zone =
        1.0 - smoothstep(0.0, 0.28, min(border.x, border.y));
    const float n1 = fx_value_noise1(seed ^ 0x77u, uv.x * 7.0 + uv.y * 2.7);
    const float n2 = fx_value_noise1(seed ^ 0xC3u, uv.y * 6.0 - uv.x * 3.1);
    const float swirl = n1 * n2 * 2.0 - 0.5;
    l += saturate(plate) * edge_zone * swirl * 0.9;
    l += saturate(plate) * 0.08 *
         (fx_value_noise1(seed ^ 0x1Bu, uv.x * 13.0 + uv.y * 11.0) - 0.5);

    const float2 d = uv - float2(0.5, 0.48);
    const float r = length(d) * 1.55;
    l *= 1.0 - saturate(vign) * 0.75 * smoothstep(0.45, 1.05, r);

    const float3 tint = lerp(float3(0.90, 0.95, 1.0),
                             float3(1.0, 0.87, 0.70), saturate(tone));
    const float3 outc = fx_srgb_eotf(saturate(saturate(l) * tint));

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
