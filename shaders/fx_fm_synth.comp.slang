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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float freq;    // cycles across the frame
    float fm;
    float pm;
    float depth;   // warp displacement px, or ring-mod depth
    float speed;   // hz
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D phase;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

static const float kTau = 6.28318530718;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float t = (fps > 0.0 ? float(frame) / fps : 0.0) * speed * kTau;
    const float l = fx_luma(fx_encoded(base));
    const uint m = uint(mode + 0.5);

    const float2 axis = m == 1u ? float2(1.0 / size.x, 0.0) : float2(0.0, 1.0 / size.y);
    const float ph = phase.SampleLevel(uv, 0).a + l * pm * 2.0;
    const float2 next = clamp(uv + axis, 0.0, 1.0);
    const float2 prev = clamp(uv - axis, 0.0, 1.0);
    const float dp = abs(phase.SampleLevel(next, 0).a - phase.SampleLevel(prev, 0).a +
                         (fx_enc_luma(input, next) - fx_enc_luma(input, prev)) * pm * 2.0) * 0.5;
    const float carrier =
        sin(ph * kTau + t) * (1.0 - smoothstep(0.25, 0.5, dp));

    float3 outc;
    float out_a = base.a;
    if (m == 2u) {
        const float k = saturate(depth / 64.0);
        outc = base.rgb * lerp(1.0, 0.5 + 0.5 * carrier, k);
    } else {
        float2 suv = uv;
        if (m == 1u) suv.y = uv.y + carrier * depth / canvas_h;
        else suv.x = uv.x + carrier * depth / canvas_w;
        const float4 s = fx_sample_border(input, suv);
        outc = s.rgb;
        out_a = s.a;
    }

    const float4 result = float4(outc, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
