// FM Synth (video-synth): a scanline carrier oscillator whose frequency
// and phase are MODULATED BY IMAGE LUMA — the Sandin/LZX move. Warp modes
// displace along the scan axis by the modulated carrier; ring-mod mode
// multiplies the image by the carrier instead (contour-band interference).
// Timeline-clocked, deterministic.
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
    float freq;    // carrier cycles across the frame
    float fm;      // luma -> frequency modulation depth
    float pm;      // luma -> phase modulation depth
    float depth;   // warp displacement px / ring-mod depth
    float speed;   // carrier drift, hz
    float mode;    // 0 h warp, 1 v warp, 2 ring mod
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float kTau = 6.28318530718;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float t = (fps > 0.0 ? float(frame) / fps : 0.0) * speed * kTau;
    const float l = fx_luma(base.rgb);
    const uint m = uint(mode + 0.5);

    // The carrier: instantaneous frequency swings with luma (FM), phase
    // kicks with luma (PM). Scan coordinate follows the mode axis.
    const float scan = m == 1u ? uv.x : uv.y;
    const float inst_freq = freq * (1.0 + fm * (l - 0.5) * 2.0);
    const float carrier =
        sin(scan * inst_freq * kTau + l * pm * kTau * 2.0 + t);

    float3 outc;
    if (m == 2u) {
        // Ring mod: multiply by the carrier remapped to 0..1 — bands of
        // the image survive where the carrier peaks, invert-ish nulls
        // where it dips. depth blends toward full modulation.
        const float k = saturate(depth / 64.0);
        outc = base.rgb * lerp(1.0, 0.5 + 0.5 * carrier, k * 2.0);
    } else {
        float2 suv = uv;
        if (m == 1u) suv.y = uv.y + carrier * depth / size.y;
        else suv.x = uv.x + carrier * depth / size.x;
        outc = input.SampleLevel(clamp(suv, 0.0, 1.0), 0).rgb;
    }

    const float4 result = float4(outc, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
