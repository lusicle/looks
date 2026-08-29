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
    float amount;   // max displacement, px
    float scale;    // noise cell size, px
    float speed;    // hz
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float vnoise2(uint s, float2 p, uint t)
{
    const float2 cell = floor(p);
    const float2 f = p - cell;
    const uint x0 = uint(int(cell.x) + 100000);
    const uint y0 = uint(int(cell.y) + 100000);
    const float a = fx_hash01(fx_hash4(s, t, x0, y0));
    const float b = fx_hash01(fx_hash4(s, t, x0 + 1u, y0));
    const float c = fx_hash01(fx_hash4(s, t, x0, y0 + 1u));
    const float d = fx_hash01(fx_hash4(s, t, x0 + 1u, y0 + 1u));
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Cross-fade two time slices to keep the motion frame-quantized.
float psi(uint s, float2 p, uint t0, float tf)
{
    return lerp(vnoise2(s, p, t0), vnoise2(s, p, t0 + 1u), tf);
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

    const float t = (fps > 0.0 ? float(frame) / fps : 0.0) * speed;
    const uint t0 = uint(floor(t));
    const float raw = t - floor(t);
    const float tf = raw * raw * (3.0 - 2.0 * raw);

    const float2 p = (float2(tid.xy) + 0.5) / max(scale, 1.0);
    const float eps = 0.35;
    const float dx = psi(seed, p + float2(eps, 0.0), t0, tf) -
                     psi(seed, p - float2(eps, 0.0), t0, tf);
    const float dy = psi(seed, p + float2(0.0, eps), t0, tf) -
                     psi(seed, p - float2(0.0, eps), t0, tf);
    const float2 vel = float2(dy, -dx) * (1.0 / (2.0 * eps));

    const float2 suv = clamp(uv + vel * amount * 2.0 / size, 0.0, 1.0);
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
