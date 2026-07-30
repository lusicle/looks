// Reeded glass: fluted bathroom-window refraction. Each flute compresses
// its slice of the image toward the flute center (with a slight cylinder
// bulge), softens toward the flute edges, and drops a dark seam plus a
// faint specular line where the flutes meet.
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
    float flutes;    // flute width, px
    float refract;   // compression + bulge strength
    float soften;    // edge-of-flute blur
    float mode;      // 0 vertical flutes, 1 horizontal
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

    const bool horiz = mode >= 0.5;
    const float p = horiz ? float(tid.y) : float(tid.x);
    const float axis_len = horiz ? size.y : size.x;
    const float fw = max(flutes, 4.0);
    const float fi = floor(p / fw);
    const float local = (p - (fi + 0.5) * fw) / fw;   // -0.5 .. 0.5

    // Compress the slice toward the flute center + cylinder bulge.
    const float rf = saturate(refract);
    float sp = (fi + 0.5) * fw + local * fw * (1.0 - rf * 0.85);
    sp += sin(local * 3.14159265) * rf * fw * 0.15;
    const float su = clamp(sp / axis_len, 0.0, 1.0);

    // 3 taps along the flute axis; edges of the flute blur more.
    const float blur_px =
        saturate(soften) * fw * 0.12 * (0.35 + abs(local) * 1.6);
    const float bo = blur_px / axis_len;
    float2 uv0 = uv, uv1 = uv, uv2 = uv;
    if (horiz) {
        uv0.y = su; uv1.y = clamp(su - bo, 0.0, 1.0);
        uv2.y = clamp(su + bo, 0.0, 1.0);
    } else {
        uv0.x = su; uv1.x = clamp(su - bo, 0.0, 1.0);
        uv2.x = clamp(su + bo, 0.0, 1.0);
    }
    float3 col = input.SampleLevel(uv0, 0).rgb * 0.5 +
                 input.SampleLevel(uv1, 0).rgb * 0.25 +
                 input.SampleLevel(uv2, 0).rgb * 0.25;

    // Dark seam between flutes, faint specular line off-center.
    const float seam = smoothstep(0.5, 0.42, abs(local));
    col *= lerp(0.5, 1.0, seam);
    const float spec = exp(-pow((abs(local) - 0.33) * 11.0, 2.0));
    col += (1.0 - col) * spec * rf * 0.08;

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
