// Anisotropic Kuwahara steered by the flow field. Picking the
// lowest-variance sector of the three is what keeps edges crisp; a plain
// mean over the whole ellipse would smear them.

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
    float radius;   // px
    float aniso;
    float boost;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D flow;    // block MVs in px
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

// Mean and variance over 12 taps in an oriented ellipse half.
void sector(float2 uv, float2 size, float2 major, float2 minor,
            float side, out float3 mean, out float variance)
{
    float3 acc = float3(0.0, 0.0, 0.0);
    float3 acc2 = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 12; ++i) {
        const float fi = (float(i) + 0.5) / 12.0;
        const float a = (fi - 0.5) * 2.6;
        const float rr = 0.25 + 0.75 * fx_hash01(fx_hash3(19u, uint(i), 7u));
        const float2 off =
            (major * cos(a) * side + minor * sin(a)) * rr;
        const float2 p = clamp(uv + off / size, 0.0, 1.0);
        const float3 c = input.SampleLevel(p, 0).rgb;
        acc += c;
        acc2 += c * c;
    }
    mean = acc / 12.0;
    const float3 var3 = max(acc2 / 12.0 - mean * mean, 0.0);
    variance = var3.x + var3.y + var3.z;
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

    const float2 mv = flow.SampleLevel(uv, 0).rg;
    const float speed = length(mv);
    const float2 dir = speed > 0.05 ? mv / speed : float2(1.0, 0.0);

    // Major axis runs along the flow.
    const float stretch =
        1.0 + aniso * (1.0 + saturate(speed * 0.15) * boost) * 1.6;
    const float2 major = dir * radius * stretch;
    const float2 minor =
        float2(-dir.y, dir.x) * radius * (1.0 - aniso * 0.55);

    float3 m0, m1, m2;
    float v0, v1, v2;
    sector(uv, size, major, minor, 1.0, m0, v0);    // ahead
    sector(uv, size, major, minor, -1.0, m1, v1);   // behind
    sector(uv, size, minor, major * 0.4, 1.0, m2, v2);   // across
    float3 outc = m0;
    float best = v0;
    if (v1 < best) { outc = m1; best = v1; }
    if (v2 < best) { outc = m2; }

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
