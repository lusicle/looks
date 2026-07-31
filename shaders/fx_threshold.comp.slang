// Threshold (docs/flow_canvas.md v5.5 primitives): soft-knee luma
// threshold to black & white — the matte/print primitive Photocopy and
// Halftone bury inside their stacks. Display-referred core.
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
    float soft;     // knee width around the threshold
    float invert;
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

    const float l = fx_luma(fx_srgb_oetf(saturate(base.rgb)));
    const float knee = max(soft, 0.001) * 0.5;
    float v = smoothstep(threshold - knee, threshold + knee, l);
    v = lerp(v, 1.0 - v, saturate(invert));

    const float4 result =
        float4(fx_srgb_eotf(float3(v, v, v)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
