// Gauge / gate masks (spec §6.8): the black projection gate for Super 8,
// 16mm, 1.85 flat, and 2.39 scope — rounded gate corners, soft edge. The
// mask multiplies (it is an aperture, so it runs in linear light).
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
    float gauge;   // 0 = Super 8 (1.33), 1 = 16mm (1.37), 2 = 1.85, 3 = 2.39
    float round_;  // corner rounding
    float soft;    // edge softness
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
    const float aspect = g == 0 ? 1.333 : (g == 1 ? 1.37
                                                  : (g == 2 ? 1.85 : 2.39));
    const float frame_aspect = size.x / size.y;

    // Gate half-extents in screen units (fit inside the frame).
    float2 half_ext;
    if (aspect >= frame_aspect)
        half_ext = float2(0.5, 0.5 * frame_aspect / aspect);
    else
        half_ext = float2(0.5 * aspect / frame_aspect, 0.5);

    // Rounded-rect SDF in aspect-corrected space.
    const float2 p = (uv - 0.5) * float2(frame_aspect, 1.0);
    const float2 he = half_ext * float2(frame_aspect, 1.0);
    const float r = round_ * min(he.x, he.y) * 0.5;
    const float2 q = abs(p) - he + r;
    const float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;

    const float edge = max(soft * 0.03, 0.002);
    const float gate = 1.0 - smoothstep(0.0, edge, d);

    const float4 result = float4(base.rgb * gate, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
