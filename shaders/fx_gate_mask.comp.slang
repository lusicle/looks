// The gate multiplies in linear light. It is an aperture, not paint.

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
    float gauge;
    float round_;
    float soft;
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

    const int g = int(gauge + 0.5);
    const float frame_aspect = canvas_w / canvas_h;
    const float2 p = (uv - 0.5) * float2(frame_aspect, 1.0);
    const float edge = max(soft * 0.03, 1.0 / float(height));

    float4 result;
    if (g == 4) {
        const float scale = min(0.9, frame_aspect * 107.0 / 88.0 * 0.9);
        const float2 outer = float2(88.0, 107.0) * (scale / 214.0);
        const float2 window = float2(79.0, 77.0) * (scale / 214.0);
        const float2 c = float2(0.0, -10.5 * scale / 107.0);
        const float r = round_ * window.y * 0.25;
        const float2 q = abs(p - c) - window + r;
        const float d =
            length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        const float gate = 1.0 - smoothstep(-edge * 0.5, edge * 0.5, d);
        const float2 card_q = abs(p) - outer;
        const float card_d = length(max(card_q, 0.0)) + min(max(card_q.x, card_q.y), 0.0);
        const float card = 1.0 - smoothstep(-edge * 0.5, edge * 0.5, card_d);
        const float3 border = fx_srgb_eotf(float3(0.94, 0.92, 0.87));
        const float inset = exp(-pow(abs(d) * 80.0, 2.0)) * 0.35;
        float3 col = lerp(border * (1.0 - inset), base.rgb, gate);
        result = float4(col, lerp(1.0, base.a, gate)) * card;
    } else {
        const float aspect =
            g == 0 ? 1.333 : (g == 1 ? 1.37 : (g == 2 ? 1.85 : 2.39));
        float2 half_ext;
        if (aspect >= frame_aspect)
            half_ext = float2(0.5, 0.5 * frame_aspect / aspect);
        else
            half_ext = float2(0.5 * aspect / frame_aspect, 0.5);

        const float2 he = half_ext * float2(frame_aspect, 1.0);
        const float r = round_ * min(he.x, he.y) * 0.5;
        const float2 q = abs(p) - he + r;
        const float d =
            length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        const float gate = 1.0 - smoothstep(-edge * 0.5, edge * 0.5, d);
        result = float4(base.rgb * gate, base.a);
    }
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
