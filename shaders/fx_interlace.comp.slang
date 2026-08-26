// Field weave is the honest model: odd lines show the PREVIOUS frame,
// so combing appears exactly where motion is and nowhere else - a
// still frame weaves back to itself untouched. Comb keeps the old
// constant parity shift as a deliberate artifact; lines-only just
// darkens the odd field.

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
    float shift;    // comb offset, px
    float darken;
    float mode;     // 0 field weave, 1 comb, 2 lines only
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D prev;   // previous frame (ring delay 1)
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
    const bool odd = (tid.y & 1u) != 0u;

    float4 result;
    if (m == 0) {
        result = odd ? prev.SampleLevel(uv, 0) : base;
    } else if (m == 1) {
        const float field = odd ? 1.0 : -1.0;
        const float parity = (frame & 1u) != 0u ? 1.0 : -1.0;
        const float2 suv = float2(
            clamp(uv.x + field * parity * shift / size.x, 0.0, 1.0),
            uv.y);
        result = input.SampleLevel(suv, 0);
    } else {
        result = base;
    }
    if (odd)
        result.rgb *= 1.0 - saturate(darken);

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
