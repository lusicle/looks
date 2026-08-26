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
    float amount;
    float start;    // radius where softness begins
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
    const float r = length(c) * 1.155;   // about 1 at the 16:9 corner
    const float t = saturate((r - start) / max(1.0 - start, 1.0e-3));
    const float radius = t * t * amount * 14.0;

    float3 outc = base.rgb;
    float out_a = base.a;
    if (radius > 0.25) {
        // Astigmatism squeezes the sampling ellipse onto the tangential
        // axis, which is what smears corners into arcs.
        const float2 rad_dir = r > 1.0e-4 ? c / length(c) : float2(1.0, 0.0);
        const float2 tan_dir = float2(-rad_dir.y, rad_dir.x);
        const float tan_r = radius * (1.0 + astig * 1.2);
        const float rad_r = radius * (1.0 - astig * 0.75);
        float3 acc = base.rgb;
        float acc_a = base.a;
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
            const float4 s = input.SampleLevel(p, 0);
            acc += s.rgb * wgt;
            acc_a += s.a * wgt;
            wsum += wgt;
        }
        outc = acc / wsum;
        out_a = acc_a / wsum;
        outc *= 1.0 - t * amount * 0.25;
    }

    const float4 result = float4(outc, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
