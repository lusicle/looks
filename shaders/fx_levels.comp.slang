// Levels (docs/flow_canvas.md v5.5 primitives): input black/white remap,
// gamma, output range — the plain tone node. Display-referred core.
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
    float black;       // input black point
    float white;       // input white point
    float gamma_v;     // midtone gamma (>1 brightens)
    float out_black;   // output range
    float out_white;
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
    const float span = max(white - black, 0.001);
    float3 c = saturate((enc - black) / span);
    c = pow(c, 1.0 / clamp(gamma_v, 0.2, 5.0));
    c = out_black + c * (out_white - out_black);

    const float4 result = float4(fx_srgb_eotf(saturate(c)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
