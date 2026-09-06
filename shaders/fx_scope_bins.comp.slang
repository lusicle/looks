#include "fx_common.slang"

[[vk::push_constant]]
cbuffer Push { uint width; uint height; uint mode; float gain; };
[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> bins;

void add_sample(float2 position, float3 color)
{
    if (any(position < 0.0) || any(position > 1.0)) return;
    uint2 p = uint2(min(position * 255.0, 255.0));
    InterlockedAdd(bins[p], 1u);
    InterlockedAdd(bins[p + uint2(256, 0)], uint(saturate(color.r) * 31.0));
    InterlockedAdd(bins[p + uint2(512, 0)], uint(saturate(color.g) * 31.0));
    InterlockedAdd(bins[p + uint2(768, 0)], uint(saturate(color.b) * 31.0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float4 source = input.SampleLevel(uv, 0);
    if (source.a <= 0.0) return;
    float3 color = fx_straight(source);
    float3 enc = fx_srgb_oetf(saturate(color));
    if (mode == 0) add_sample(float2(uv.x, 1.0 - fx_luma(enc) * gain), color);
    else if (mode == 1) {
        for (uint ch = 0; ch < 3; ++ch) {
            float3 tint = ch == 0 ? float3(1, 0.25, 0.2) : ch == 1 ? float3(0.25, 1, 0.3) : float3(0.3, 0.4, 1);
            add_sample(float2((uv.x + float(ch)) / 3.0, 1.0 - enc[ch] * gain), tint);
        }
    } else {
        float2 cbcr = fx_rgb_to_cbcr709(enc, fx_luma(enc));
        add_sample(float2(cbcr.x, -cbcr.y) * gain * 0.9 + 0.5, color);
    }
}
