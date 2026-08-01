// Sharpen (primitive): clean unsharp mask — a separable-ish 9-tap
// gaussian estimate subtracted from the pixel, added back scaled. The
// deliberately-degraded halo look stays in Oversharpen; this one is the
// neutral tool. Taps stay bounded (9 fixed) regardless of radius.
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
    float amount;   // 0..2 unsharp gain
    float radius;   // 0.5..6 px blur radius
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 px = 1.0 / float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) * px;
    const float4 base = input.SampleLevel(uv, 0);

    // 3x3 gaussian ring at `radius` spacing — bilinear taps make the
    // footprint scale continuously without extra samples.
    const float2 r = px * max(radius, 0.01);
    float3 blur = base.rgb * 0.25;
    blur += input.SampleLevel(uv + float2(r.x, 0.0), 0).rgb * 0.125;
    blur += input.SampleLevel(uv - float2(r.x, 0.0), 0).rgb * 0.125;
    blur += input.SampleLevel(uv + float2(0.0, r.y), 0).rgb * 0.125;
    blur += input.SampleLevel(uv - float2(0.0, r.y), 0).rgb * 0.125;
    blur += input.SampleLevel(uv + r, 0).rgb * 0.0625;
    blur += input.SampleLevel(uv - r, 0).rgb * 0.0625;
    blur += input.SampleLevel(uv + float2(r.x, -r.y), 0).rgb * 0.0625;
    blur += input.SampleLevel(uv + float2(-r.x, r.y), 0).rgb * 0.0625;

    const float3 sharpened =
        max(base.rgb + (base.rgb - blur) * amount, 0.0);

    const float4 wetted =
        lerp(base, float4(sharpened, base.a), saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
