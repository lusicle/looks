// Composite video artifacts (spec §6.7): chroma bleed/delay (color smeared
// right of where it belongs), dot crawl (checkerboard climbing chroma
// edges), and rainbow shimmer (cross-color on fine luma detail). Runs in
// encoded space — these live in the signal domain, not the scene.
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
    float dot_crawl;
    float rainbow;
    float chroma_delay;   // px
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float3 enc_at(float2 uv)
{
    return fx_srgb_oetf(saturate(input.SampleLevel(uv, 0).rgb));
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

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float y0 = fx_luma(enc);

    // Chroma delay: luma stays put, color arrives late (from the left) and
    // slightly blurred.
    const float dx = chroma_delay / size.x;
    const float3 delayed =
        (enc_at(uv - float2(dx, 0.0)) + enc_at(uv - float2(dx * 0.5, 0.0))) *
        0.5;
    float3 col = delayed - fx_luma(delayed) + y0;

    // Chroma edge strength drives dot crawl.
    const float2 e = 1.5 / size;
    const float3 cl = enc_at(uv - float2(e.x, 0.0));
    const float3 cr = enc_at(uv + float2(e.x, 0.0));
    const float3 chroma_l = cl - fx_luma(cl);
    const float3 chroma_r = cr - fx_luma(cr);
    const float chroma_edge = saturate(length(chroma_r - chroma_l) * 3.0);
    const float crawl_pattern =
        float((tid.x + tid.y + frame) & 1u) * 2.0 - 1.0;
    col += crawl_pattern * chroma_edge * dot_crawl * 0.12;

    // Rainbow shimmer: fine horizontal luma detail beats with the (fake)
    // subcarrier and comes back as drifting hue.
    const float lum_l = fx_luma(enc_at(uv - float2(e.x, 0.0)));
    const float lum_r = fx_luma(enc_at(uv + float2(e.x, 0.0)));
    const float detail = saturate(abs(lum_r - lum_l) * 4.0);
    const float phase =
        float(tid.x) * 0.9 + float(tid.y) * 0.3 + float(frame) * 1.7;
    const float3 shimmer = float3(sin(phase), sin(phase + 2.094),
                                  sin(phase + 4.189));
    col += shimmer * detail * rainbow * 0.10;

    const float4 result = float4(fx_srgb_eotf(saturate(col)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
