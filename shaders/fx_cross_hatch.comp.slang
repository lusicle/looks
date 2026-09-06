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
    const float2 px = uv * float2(canvas_w, canvas_h);
    const float footprint = canvas_w / float(width);

    const float l = fx_luma(fx_encoded(base));
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
        const float press = saturate((thresh - l) / max(thresh, 1.0e-3));
        const float half_w = press * 1.1;
        const float duty = saturate(2.0 * half_w / pitch);
        const float span = footprint * (abs(ca) + abs(sa)) / pitch;
        const float coord = (across + tremor) / pitch - 0.5 + duty * 0.5;
        const float lo = coord - span * 0.5;
        const float hi = coord + span * 0.5;
        const float coverage = ((floor(hi) - floor(lo)) * duty + min(frac(hi), duty) - min(frac(lo), duty)) / span;
        const float stroke = lerp(duty, saturate(coverage),
                                 smoothstep(1.5, 3.0, pitch / footprint));
        paper *= 1.0 - saturate(stroke * ink);
    }

    const float3 drawn =
        lerp(float3(paper, paper, paper), saturate(fx_straight(base)) * paper, 0.15);

    const float4 result = float4(drawn * base.a, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
