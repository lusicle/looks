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
    float op;
    float radius;
    float shape;
    float angle;
    float channels;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

bool in_kernel(int2 d, int r, float shp, float ang)
{
    if (shp < 0.5)
        return d.x * d.x + d.y * d.y <= r * r;
    if (shp < 1.5)
        return true;
    if (shp < 2.5)
        return d.x == 0 || d.y == 0;
    const float2 dir = float2(cos(ang), sin(ang));
    const float along = float(d.x) * dir.x + float(d.y) * dir.y;
    const float across = float(d.x) * -dir.y + float(d.y) * dir.x;
    return abs(across) <= 0.6 && abs(along) <= float(r) + 0.5;
}

float4 sweep(float2 base_uv, float2 size, int r, bool grow)
{
    float4 acc = input.SampleLevel(base_uv, 0);
    for (int y = -r; y <= r; ++y)
    {
        for (int x = -r; x <= r; ++x)
        {
            if (!in_kernel(int2(x, y), r, shape, angle))
                continue;
            const float2 uv =
                clamp(base_uv + float2(x, y) / size, 0.0, 1.0);
            const float4 s = input.SampleLevel(uv, 0);
            if (channels < 0.5)
            {
                const float sl = fx_luma(s.rgb);
                const float al = fx_luma(acc.rgb);
                if (grow ? sl > al : sl < al)
                    acc = s;
            }
            else if (channels < 1.5)
            {
                acc.rgb = grow ? max(acc.rgb, s.rgb) : min(acc.rgb, s.rgb);
                acc.a = grow ? max(acc.a, s.a) : min(acc.a, s.a);
            }
            else
            {
                if (grow ? s.a > acc.a : s.a < acc.a)
                    acc = s;
            }
        }
    }
    return acc;
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

    const int r = int(clamp(radius, 0.0, 6.0) + 0.5);
    float4 result = base;
    if (r > 0)
        result = sweep(uv, size, r, op < 0.5);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
