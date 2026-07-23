// Block shuffle: macroblock-level copy/stamp glitch, GPU-side — a chosen
// fraction of blocks display some OTHER block's pixels, re-rolled on the
// rate clock. The instant-gratification cousin of the Codec-Box moshes:
// no bitstream, pure block-pointer corruption. Seeded, deterministic.
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
    float block_px;   // block size
    float amount;     // fraction of blocks remapped
    float spread;     // how far a stolen block may come from
    float rate;       // re-roll rate, hz (0 = frozen pattern)
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

    const float bs = max(block_px, 4.0);
    const uint2 block = uint2(float2(tid.xy) / bs);
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = rate > 0.01 ? uint(t * rate) : 0u;

    const uint h = fx_hash4(seed ^ tick * 0x51EDu, tick, block.x, block.y);
    float4 result = base;
    if (fx_hash01(h) < amount) {
        // Steal from a block up to spread*8 blocks away (wrapped).
        const float reach = spread * 8.0 + 1.0;
        const float ox =
            (fx_hash01(fx_hash(h ^ 0x111u)) - 0.5) * 2.0 * reach;
        const float oy =
            (fx_hash01(fx_hash(h ^ 0x222u)) - 0.5) * 2.0 * reach;
        const float2 src_px =
            float2(tid.xy) + float2(ox, oy) * bs;
        const float2 suv = frac(src_px / size);
        result = input.SampleLevel(suv, 0);
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
