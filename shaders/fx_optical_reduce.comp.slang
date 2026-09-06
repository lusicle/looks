#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push { uint width; uint height; float dx; float dy; float threshold; };
[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float4 sum = 0.0;
    for (int y = -1; y <= 1; y += 2) {
        for (int x = -1; x <= 1; x += 2) {
            float4 s = fx_sample_border(input, uv + float2(x * dx, y * dy));
            if (threshold >= 0.0) {
                float l = fx_luma(s.rgb);
                s *= max(l - threshold, 0.0) / max(l, 1e-5);
            }
            sum += s;
        }
    }
    output[tid.xy] = sum * 0.25;
}
