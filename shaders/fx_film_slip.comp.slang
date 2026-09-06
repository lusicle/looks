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
    float slip;    // 0..1 of a frame height
    float speed;   // hz
    float blend;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D previous;
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float offset = frac(slip + t * speed);

    float4 rolled = 0.0;
    const float travel = speed * 0.5 / max(fps, 1.0);
    for (int i = 0; i < 8; ++i) {
        const float y = uv.y + offset - travel * (float(i) + 0.5) / 8.0;
        const float2 suv = float2(uv.x, frac(y));
        rolled += (y >= 1.0 ? previous.SampleLevel(suv, 0) : input.SampleLevel(suv, 0)) * 0.125;
    }
    float3 col = rolled.rgb;
    float col_a = rolled.a;

    if (blend > 0.001) {
        const float2 guv = float2(uv.x, frac(uv.y + offset + 0.5));
        const float4 ghost = previous.SampleLevel(guv, 0);
        col = lerp(col, ghost.rgb, blend * 0.5);
        col_a = lerp(col_a, ghost.a, blend * 0.5);
    }

    if (offset > 0.002 && offset < 0.998) {
        const float seam = 1.0 - offset;
        const float d = min(abs(uv.y - seam),
                            min(abs(uv.y - seam + 1.0),
                                abs(uv.y - seam - 1.0)));
        const float half_h = 0.022;
        const float bar = 1.0 - smoothstep(half_h * 0.55, half_h, d);
        col *= 1.0 - bar * 0.92;
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
