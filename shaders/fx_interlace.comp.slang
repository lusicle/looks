// The comb parity shift is a deliberate artifact.

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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float shift;    // comb offset, px
    float darken;
    float mode;
    float standard;
    float field_order;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;   // the previous frame, ring delay 1
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

    const int m = int(mode + 0.5);
    const float raster = standard < 0.5 ? 480.0 : 576.0;
    const float footprint = raster / float(height);
    const float line0 = uv.y * raster - footprint * 0.5;
    const float line1 = line0 + footprint;
    const float integral0 = floor(line0 * 0.5) + max(frac(line0 * 0.5) * 2.0 - 1.0, 0.0);
    const float integral1 = floor(line1 * 0.5) + max(frac(line1 * 0.5) * 2.0 - 1.0, 0.0);
    const float odd = saturate((integral1 - integral0) / footprint);
    const uint field = uint(float(frame) / max(fps, 1.0) * (standard < 0.5 ? 59.94 : 50.0));
    const float older = ((field + uint(field_order)) & 1u) != 0u ? 1.0 - odd : odd;

    float4 result;
    if (m == 0) {
        result = lerp(base, prev.SampleLevel(uv, 0), older);
    } else if (m == 1) {
        const float parity = (frame & 1u) != 0u ? 1.0 : -1.0;
        const float2 delta = float2(parity * shift / canvas_w, 0.0);
        result = lerp(input.SampleLevel(uv - delta, 0), input.SampleLevel(uv + delta, 0), odd);
    } else {
        result = base;
    }
    result.rgb *= 1.0 - saturate(darken) * odd;

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
