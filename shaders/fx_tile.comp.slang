// Tile: grid repeat with mirror alternation (the TD Tile TOP). Mirroring
// makes edges continuous so the grid reads kaleidoscopic rather than cut.
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
    float cols;
    float rows_n;
    float mirror;   // 1 = alternate-flip tiles
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float2 n = float2(max(cols, 1.0), max(rows_n, 1.0));
    const float2 g = uv * n;
    float2 f = frac(g);
    if (mirror >= 0.5) {
        const int2 cell = int2(floor(g));
        if ((cell.x & 1) != 0) f.x = 1.0 - f.x;
        if ((cell.y & 1) != 0) f.y = 1.0 - f.y;
    }
    const float3 outc = input.SampleLevel(clamp(f, 0.0, 1.0), 0).rgb;

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
