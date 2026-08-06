// Emits an ordinary grayscale image, so the result can feed a matte port
// or simply keep going through the chain. Output mode cutout instead
// carves the key into premultiplied alpha - the input survives, gated.

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
    float mode;       // 0 luma, 1 brightness key, 2 chroma key
    float key_c;      // luma center, or key hue in chroma mode
    float key_range;
    float black_pt;
    float white_pt;
    float gamma_v;
    float invert_amt;
    float out_mode;   // 0 grayscale matte image, 1 premultiplied cutout
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float3 enc = fx_srgb_oetf(saturate(base.rgb));
    const float l = fx_luma(enc);
    const uint m = uint(mode + 0.5);
    float v;
    if (m == 1u)
    {
        const float r = max(key_range, 0.01);
        v = saturate(1.0 - abs(l - key_c) / r);
    }
    else if (m == 2u)
    {
        // Distance in chroma only, with luma removed from both sides, so
        // shading inside the keyed color survives.
        const float3 key_rgb = fx_srgb_oetf(saturate(
            0.5 + 0.5 * cos(6.2831853 * (key_c + float3(0.0, -1.0 / 3.0,
                                                        1.0 / 3.0)))));
        const float3 ec = enc - fx_luma(enc);
        const float3 kc = key_rgb - fx_luma(key_rgb);
        const float d = length(ec - kc);
        v = saturate(d / max(key_range, 0.01));
    }
    else
    {
        v = l;
    }
    const float span = max(white_pt - black_pt, 1e-4);
    v = saturate((v - black_pt) / span);
    v = pow(v, max(gamma_v, 0.01));
    v = lerp(v, 1.0 - v, saturate(invert_amt));

    const float4 result =
        int(out_mode + 0.5) == 1
            ? base * v
            : float4(fx_srgb_eotf(float3(v, v, v)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
