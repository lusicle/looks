// Buffer glitch: framebuffer corruption two ways — stuck columns (a column
// latches and repeats one source column, like dead shift registers) and
// row shear (bands of rows slide sideways progressively down the frame,
// like a slipping row pointer). Mode picks either or both. Seeded.
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
    float mode;      // 0 stuck columns, 1 row shear, 2 both
    float amount;    // severity
    float density;   // how much of the frame is affected
    float rate;      // re-roll rate, hz (0 = frozen)
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

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint tick = rate > 0.01 ? uint(t * rate) : 0u;
    const uint m = uint(mode + 0.5);
    float2 suv = uv;

    if (m != 1u) {
        // Stuck columns: 4px lanes; a stuck lane snaps its x to the lane's
        // seeded source column, duplicating it across the lane run.
        const uint lane = tid.x / 4u;
        const uint lh = fx_hash4(seed ^ 0xC01u, tick, lane, 0u);
        if (fx_hash01(lh) < density * 0.5 * amount) {
            const float src_x =
                fx_hash01(fx_hash(lh ^ 0x77u));
            // Pull from a nearby column so content still relates.
            suv.x = clamp(uv.x + (src_x - 0.5) * 0.1 * (1.0 + amount * 3.0),
                          0.0, 1.0);
            suv.x = floor(suv.x * size.x / 4.0) * 4.0 / size.x;
        }
    }
    if (m >= 1u) {
        // Row shear: 8px bands, displacement growing down the frame like
        // an accumulating pointer slip.
        const uint band = tid.y / 8u;
        const uint bh = fx_hash4(seed ^ 0x5EA4u, tick, band, 1u);
        if (fx_hash01(bh) < density) {
            const float grow = uv.y;   // worsens toward the bottom
            const float shift =
                (fx_value_noise1(seed ^ tick * 3u, float(band) * 0.37) - 0.5)
                * amount * 160.0 * grow;
            suv.x = frac(suv.x + shift / size.x);
        }
    }

    const float4 result = input.SampleLevel(suv, 0);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
