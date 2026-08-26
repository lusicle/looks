// Encoded space: snow rides on the signal, not the scene.
//
// Weak-RF reception: impulse sparks smear into comet tails along the
// scan direction, a multipath ghost adds a delayed copy to the right
// (AGC renormalizes the sum), and the noise floor lifts everywhere.

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

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    float out_a = base.a;

    // Multipath: the delayed copy lands to the RIGHT of the direct
    // signal; AGC pulls the summed level back down.
    const float g = saturate(ghost);
    if (g > 0.001) {
        const float2 guv =
            clamp(uv - float2(ghost_px, 0.0) / size, 0.0, 1.0);
        const float4 ghost4 = input.SampleLevel(guv, 0);
        const float3 ghost_enc = fx_srgb_oetf(saturate(ghost4.rgb));
        enc = (enc + ghost_enc * g * 0.8) / (1.0 + g * 0.55);
        out_a = max(out_a, ghost4.a * g * 0.8);
    }

    // Impulse sparks with comet tails: a hit at x smears over the
    // following ~dozen pixels of the scan, decaying by its own tail
    // length. Dark hits are shorter drop-out combs.
    float spark = 0.0;
    float dark = 0.0;
    [unroll]
    for (uint k = 0u; k < 12u; ++k) {
        const uint h = fx_hash4(seed, frame, tid.x - k, tid.y);
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
    // The floor: broadband grain under everything, faintly streaky.
    const uint fh = fx_hash4(seed ^ 0x5A0Fu, frame, tid.x >> 1, tid.y);
    const float fl = (fx_hash01(fh) - 0.5) * saturate(floor_amt) * 0.5;

    enc = saturate(enc + fl + spark * amount * 1.5 - dark * amount * 0.9);
    // RF sparks are their own light.
    out_a = max(out_a, saturate(spark * amount * 1.5));
    out_a = max(out_a, saturate(abs(fl) * 2.0));

    const float4 result = float4(fx_srgb_eotf(enc), out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
