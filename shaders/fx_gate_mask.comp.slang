// The gate multiplies in linear light: it is an aperture, not a paint.

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
    float gauge;   // 0 super 8, 1 16mm, 2 1.85, 3 2.39, 4 instant print
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
    const float frame_aspect = size.x / size.y;
    const float2 p = (uv - 0.5) * float2(frame_aspect, 1.0);
    const float edge = max(soft * 0.03, 0.002);

    float4 result;
    if (g == 4) {
        // The window sits high, which is what gives the fat bottom border.
        const float hs = 0.365;
        const float2 c = float2(0.0, -0.055);
        const float r = round_ * hs * 0.25 + 0.004;
        const float2 q = abs(p - c) - float2(hs, hs) + r;
        const float d =
            length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        const float gate = 1.0 - smoothstep(0.0, edge, d);
        const float3 border = fx_srgb_eotf(float3(0.94, 0.92, 0.87));
        const float inset = exp(-pow(abs(d) * 80.0, 2.0)) * 0.35;
        float3 col = lerp(border * (1.0 - inset), base.rgb, gate);
        result = float4(col, base.a);
    } else {
        const float aspect =
            g == 0 ? 1.333 : (g == 1 ? 1.37 : (g == 2 ? 1.85 : 2.39));
        // Fit the gate inside the frame, whichever way it is limited.
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
        const float gate = 1.0 - smoothstep(0.0, edge, d);
        result = float4(base.rgb * gate, base.a);
    }
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
