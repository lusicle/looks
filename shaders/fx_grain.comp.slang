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
    float amount;
    float size;        // cell size, px
    float color;
    float mode;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

float grain_noise(uint h)
{
    return (fx_hash01(fx_hash(h)) + fx_hash01(fx_hash(h ^ 0x1234567u)) +
            fx_hash01(fx_hash(h ^ 0x89ABCDEu)) + fx_hash01(fx_hash(h ^ 0xF0F0F0Fu)) - 2.0) * 1.7320508;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    if (amount <= 0.0 || wet <= 0.0 || opacity <= 0.0) { output[tid.xy] = base; return; }

    const bool ccd = mode >= 0.5;
    const float2 material_px = uv * float2(canvas_w, canvas_h);
    const uint2 sensor_px = uint2(material_px);
    const float footprint = canvas_w / float(width);
    float3 grain;
    if (ccd) {
        const uint h_mono = fx_hash4(seed, frame, sensor_px.x, sensor_px.y);
        const float mono = grain_noise(h_mono);
        grain = float3(mono, mono, mono);
        const float3 chroma = float3(
            grain_noise(h_mono ^ 0x51A3u),
            grain_noise(h_mono ^ 0x912Bu),
            grain_noise(h_mono ^ 0xC734u));
        const float mix_color = saturate(color);
        grain = lerp(grain, chroma, mix_color) / sqrt((1.0 - mix_color) * (1.0 - mix_color) + mix_color * mix_color);
    } else {
        const float cell_size = max(size, 1.0);
        const uint fs = fx_hash3(seed, frame, 0x6A1u);
        const float2 pm = material_px / cell_size;
        const float g_mono =
            (fx_vnoise2(fs, pm) * 0.65 +
             fx_vnoise2(fs ^ 0x2Cu, pm * 2.3) * 0.35 - 0.5) * 2.2;
        const float3 layer_scale = float3(1.0, 1.12, 1.4);
        const float3 layer_amp = float3(0.9, 1.0, 1.45);
        float3 clumps;
        [unroll]
        for (int ch = 0; ch < 3; ++ch) {
            const uint cs = fx_hash(fs ^ (0x51u + uint(ch) * 0x9E3u));
            const float2 pc = pm / layer_scale[ch];
            clumps[ch] =
                (fx_vnoise2(cs, pc) * 0.65 +
                 fx_vnoise2(cs ^ 0x77u, pc * 2.3) * 0.35 - 0.5) * 2.2 *
                layer_amp[ch];
        }
        grain = lerp(float3(g_mono, g_mono, g_mono), clumps,
                     saturate(color));
    }

    const float3 linear = saturate(fx_straight(base));
    const float filter = min(1.0, (ccd ? 1.0 : max(size, 1.0)) / footprint);
    float3 grained;
    if (ccd) {
        const float row = grain_noise(fx_hash3(seed, 0u, sensor_px.y));
        const float3 sigma = amount * sqrt(linear * 0.0144 + 0.000016);
        grained = saturate(linear + grain * sigma * filter + row * amount * 0.004 * sqrt(filter));
    } else {
        const float3 density = -log(max(linear, 1e-6));
        const float3 variation = grain * amount * 0.65 * sqrt(density) * filter;
        grained = saturate(linear * exp(-variation));
    }

    const float4 result = float4(grained * base.a, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
