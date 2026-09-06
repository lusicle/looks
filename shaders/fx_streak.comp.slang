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
    float length_px;
    float angle;
    float decay;       // per tap
    float threshold;
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
    if (length_px <= 0.0 || wet <= 0.0 || opacity <= 0.0) { output[tid.xy] = base; return; }

    const float2 dir = float2(cos(angle), sin(angle));
    const bool glow_mode = threshold > 0.001;

    float2 travel = abs(dir) * length_px * size / float2(canvas_w, canvas_h);
    uint taps = uint(clamp(ceil(max(travel.x, travel.y)), 1.0, 256.0));
    float falloff = pow(clamp(decay, 0.5, 0.99), 24.0 / float(taps));
    float4 acc = 0.0;
    float wsum = 0.0;
    float w = sqrt(falloff);
    for (uint i = 0; i < taps; ++i) {
        const float d = (float(i) + 0.5) / float(taps) * length_px;
        const float2 p = uv - dir * d / float2(canvas_w, canvas_h);
        float4 c = glow_mode ? fx_sample_border(input, p) :
            input.SampleLevel(clamp(p, 0.0, 1.0), 0);
        if (glow_mode) {
            float l = fx_luma(c.rgb);
            c *= max(l - threshold, 0.0) / max(l, 1e-5);
        }
        acc += c * w;
        wsum += w;
        w *= falloff;
    }
    float4 result = acc / wsum;
    if (glow_mode) {
        acc *= 3.0 / float(taps);
        result = float4(base.rgb + acc.rgb, max(base.a, saturate(acc.a)));
    }
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
