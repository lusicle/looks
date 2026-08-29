// Work in display-referred space. The drawing is ink on paper.

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
    float spacing;   // px
    float layers;    // hatch directions, 1 to 4
    float ink;
    float wobble;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float kAngles[4] = {0.7854, -0.7854, 0.0, 1.5708};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float2 px = float2(tid.xy);

    const float l = fx_luma(fx_srgb_oetf(saturate(base.rgb)));
    const int n = clamp(int(layers + 0.5), 1, 4);
    const float pitch = max(spacing, 2.0);

    float paper = 1.0;
    for (int i = 0; i < n; ++i) {
        const float thresh = 1.0 - (float(i) + 0.55) / (float(n) + 0.6);
        if (l >= thresh) continue;
        const float a = kAngles[i];
        const float ca = cos(a), sa = sin(a);
        const float along = px.x * ca + px.y * sa;
        const float across = -px.x * sa + px.y * ca;
        const float tremor =
            (fx_value_noise1(seed ^ uint(i) * 91u, along * 0.05) - 0.5) *
            wobble * pitch * 0.8;
        const float d =
            abs(frac((across + tremor) / pitch) - 0.5) * pitch;
        const float press = saturate((thresh - l) / max(thresh, 1.0e-3));
        const float half_w = 0.35 + press * 0.75;
        const float stroke = 1.0 - smoothstep(half_w - 0.4, half_w + 0.4, d);
        paper = min(paper, 1.0 - stroke * ink);
    }

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float3 drawn =
        lerp(float3(paper, paper, paper), enc * paper, 0.15);

    const float4 result = float4(fx_srgb_eotf(saturate(drawn)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
