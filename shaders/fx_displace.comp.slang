// Displacement by luma or second input (spec §6.2). The displacement
// field always comes from `map`: the engine binds the effect input itself
// (self-luma) or, with map_mode = 1 and a mask reference, the mask's
// grayscale — whose source can be another layer, an external video, or a
// generator, each with its own mini chain (spec §8). Mode 0 shifts along
// a fixed direction scaled by (map luma - 0.5); mode 1 pushes along the
// local map-luma gradient (relief-like bulge).
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
    float amount;     // px at full luma swing
    float angle;      // direction for mode 0
    float mode;       // 0 = directional, 1 = gradient
    float map_mode;   // 0 = self, 1 = mask (consumed by the graph compiler)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D map;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    float2 offset_px;
    if (mode < 0.5) {
        const float l = fx_luma(saturate(map.SampleLevel(uv, 0).rgb)) - 0.5;
        offset_px = float2(cos(angle), sin(angle)) * l * amount;
    } else {
        const float2 e = 2.0 / size;
        const float lx =
            fx_luma(saturate(map.SampleLevel(uv + float2(e.x, 0), 0).rgb)) -
            fx_luma(saturate(map.SampleLevel(uv - float2(e.x, 0), 0).rgb));
        const float ly =
            fx_luma(saturate(map.SampleLevel(uv + float2(0, e.y), 0).rgb)) -
            fx_luma(saturate(map.SampleLevel(uv - float2(0, e.y), 0).rgb));
        offset_px = float2(lx, ly) * amount;
    }

    const float2 suv = clamp(uv + offset_px / size, 0.0, 1.0);
    const float4 result = input.SampleLevel(suv, 0);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
