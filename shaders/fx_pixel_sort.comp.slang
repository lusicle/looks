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
    float threshold;   // band low edge, encoded luma
    float range;
    float length_px;   // max walked per side
    float direction;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float luma_at(float2 uv)
{
    return fx_luma(fx_srgb_oetf(saturate(input.SampleLevel(uv, 0).rgb)));
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

    const float lo = threshold;
    const float hi = threshold + max(range, 0.01);
    const float l0 = luma_at(uv);
    if (l0 < lo || l0 > hi) {
        output[tid.xy] = base;
        return;
    }

    const float2 axis = direction >= 0.5 ? float2(0.0, 1.0 / size.y)
                                         : float2(1.0 / size.x, 0.0);
    const int max_walk = int(clamp(length_px, 8.0, 256.0));

    int back = 0;
    float3 dark = base.rgb, bright = base.rgb;
    float dark_a = base.a, bright_a = base.a;
    float dark_l = l0, bright_l = l0;
    for (int i = 1; i <= max_walk; ++i) {
        const float2 p = uv - axis * float(i);
        if (any(p < 0.0)) break;
        const float l = luma_at(p);
        if (l < lo || l > hi) break;
        back = i;
        const float4 c = input.SampleLevel(p, 0);
        if (l < dark_l) { dark_l = l; dark = c.rgb; dark_a = c.a; }
        if (l > bright_l) { bright_l = l; bright = c.rgb; bright_a = c.a; }
    }
    int fwd = 0;
    for (int i = 1; i <= max_walk; ++i) {
        const float2 p = uv + axis * float(i);
        if (any(p > 1.0)) break;
        const float l = luma_at(p);
        if (l < lo || l > hi) break;
        fwd = i;
        const float4 c = input.SampleLevel(p, 0);
        if (l < dark_l) { dark_l = l; dark = c.rgb; dark_a = c.a; }
        if (l > bright_l) { bright_l = l; bright = c.rgb; bright_a = c.a; }
    }

    const float span = float(back + fwd);
    float3 col = base.rgb;
    float col_a = base.a;
    if (span >= 2.0) {
        const float f = float(back) / span;
        col = lerp(dark, bright, f);
        col_a = lerp(dark_a, bright_a, f);
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
