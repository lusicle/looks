// Work in encoded space. The snow rides on the signal.

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
    float ghost_px;
    float ghost;
    float floor_amt;
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
    const uint2 signal_px = uint2(uv * float2(canvas_w, canvas_h));
    const uint field = uint(float(frame) * 59.94 / max(fps, 1.0));
    const float noise_scale = min(1.0, float(width) / canvas_w);

    float3 enc = fx_encoded(base);
    float out_a = base.a;

    // The delayed copy lands to the right, so the sample uses a minus.
    const float g = saturate(ghost);
    if (g > 0.001) {
        const float2 guv =
            clamp(uv - float2(ghost_px / canvas_w, 0.0), 0.0, 1.0);
        const float4 ghost4 = input.SampleLevel(guv, 0);
        const float3 ghost_enc = fx_encoded(ghost4);
        const float coverage = out_a + ghost4.a * g * 0.8;
        enc = (enc * out_a + ghost_enc * (ghost4.a * g * 0.8)) / max(coverage, 1e-6);
        out_a = coverage / (1.0 + g * 0.8);
    }

    float spark = 0.0;
    float dark = 0.0;
    [unroll]
    for (uint k = 0u; k < 12u; ++k) {
        const uint h = fx_hash4(seed, field, signal_px.x - min(k, signal_px.x), signal_px.y);
        const float u = fx_hash01(h);
        const float fire = pow(u, 24.0);
        if (fire > 0.001) {
            const float tail =
                2.0 + 9.0 * fx_hash01(fx_hash(h ^ 0x33u));
            spark += fire * exp(-float(k) / tail);
        }
        const float d = pow(fx_hash01(fx_hash(h ^ 0x9E3779B9u)), 40.0);
        if (d > 0.001)
            dark += d * exp(-float(k) / 2.5);
    }
    spark *= noise_scale;
    dark *= noise_scale;
    const uint fh = fx_hash4(seed ^ 0x5A0Fu, field, signal_px.x >> 1, signal_px.y);
    const float fl = (fx_hash01(fh) - 0.5) * saturate(floor_amt) * 0.5 * noise_scale;

    enc = saturate(enc + fl + spark * amount * 1.5 - dark * amount * 0.9);
    // RF sparks are their own light.
    out_a = max(out_a, saturate(spark * amount * 1.5));
    out_a = max(out_a, saturate(abs(fl) * 2.0));

    const float4 result = fx_from_encoded(enc, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
