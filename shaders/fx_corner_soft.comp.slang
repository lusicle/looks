// Corner softness: cheap-lens field curvature — sharp center, blur rising
// toward the corners, with optional astigmatism (the blur ellipse stretches
// tangentially, so corners smear in arcs like a Petzval or a toy camera).
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
    float amount;   // corner blur strength
    float start;    // radius where softness begins (0..1)
    float astig;    // 0 round blur, 1 fully tangential smear
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

    const float2 c = (uv - 0.5) * float2(size.x / size.y, 1.0);
    const float r = length(c) * 1.155;   // ~1 at the 16:9 corner
    const float t = saturate((r - start) / max(1.0 - start, 1.0e-3));
    const float radius = t * t * amount * 14.0;

    float3 outc = base.rgb;
    if (radius > 0.25) {
        // Tangential/radial frame at this pixel; astigmatism squeezes the
        // sampling ellipse onto the tangential axis.
        const float2 rad_dir = r > 1.0e-4 ? c / length(c) : float2(1.0, 0.0);
        const float2 tan_dir = float2(-rad_dir.y, rad_dir.x);
        const float tan_r = radius * (1.0 + astig * 1.2);
        const float rad_r = radius * (1.0 - astig * 0.75);
        float3 acc = base.rgb;
        float wsum = 1.0;
        [unroll]
        for (int i = 0; i < 16; ++i) {
            const float fi = (float(i) + 0.5) / 16.0;
            const float a = float(i) * 2.39996;
            const float rr = sqrt(fi);
            const float2 off = (tan_dir * cos(a) * tan_r +
                                rad_dir * sin(a) * rad_r) * rr;
            const float2 p = clamp(uv + off / size, 0.0, 1.0);
            const float wgt = 1.0 - fi * 0.6;
            acc += input.SampleLevel(p, 0).rgb * wgt;
            wsum += wgt;
        }
        outc = acc / wsum;
        // A touch of corner light falloff sells the cheap glass.
        outc *= 1.0 - t * amount * 0.25;
    }

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
