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
    float radius;
    float blades;
    float rotation;
    float cat_eye;
    float depth_src;
    float focus;
    float highlight;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D aux;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

static const int kTaps = 48;

float iris_edge(float theta, float n, float rot)
{
    const float seg = 6.2831853 / max(n, 3.0);
    const float a = theta - rot;
    const float k = a - seg * floor(a / seg) - seg * 0.5;
    return cos(seg * 0.5) / max(cos(k), 1.0e-3);
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

    float depth;
    if (depth_src < 0.5)
        depth = fx_luma(base.rgb);
    else if (depth_src < 1.5)
        depth = saturate(length(uv - 0.5) * 2.0);
    else
        depth = fx_luma(aux.SampleLevel(uv, 0).rgb);

    const float coc = abs(depth - focus) * 2.0;
    const float r = radius * coc;
    if (r < 0.5)
    {
        output[tid.xy] = base;
        return;
    }

    const float2 off = uv - 0.5;
    const float edge = saturate(length(off) * 2.0) * saturate(cat_eye);
    const float2 radial = length(off) > 1.0e-4 ? normalize(off) : float2(1, 0);

    float4 acc = 0.0;
    float wsum = 0.0;
    const float golden = 2.3999632;
    for (int i = 0; i < kTaps; ++i)
    {
        const float t = (float(i) + 0.5) / float(kTaps);
        const float theta = float(i) * golden;
        float2 d = float2(cos(theta), sin(theta)) * sqrt(t) *
                   iris_edge(theta, blades, rotation);
        const float along = dot(d, radial);
        d -= radial * along * edge * 0.75;

        const float2 suv = clamp(uv + d * r / size, 0.0, 1.0);
        const float4 s = input.SampleLevel(suv, 0);
        const float wgt = 1.0 + fx_luma(s.rgb) * (highlight - 1.0);
        acc += s * wgt;
        wsum += wgt;
    }
    const float4 result = wsum > 0.0 ? acc / wsum : base;

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
