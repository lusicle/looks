// Mask extraction (spec §8): grayscale from a source (after its mini
// chain) -> levels (black/white/gamma) -> optional invert. Key modes fold
// in here: luma key and chroma key are just different grayscale functions.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint mode;         // 0 luma, 1 R, 2 G, 3 B, 4 luma key, 5 chroma key,
                       // 6 alpha, 7 flow magnitude (motion mask)
    uint invert;
    float black_point;
    float white_point;
    float gamma;
    float key_center;
    float key_range;
    float key_r;
    float key_g;
    float key_b;
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
    const float4 rgba = input.SampleLevel(uv, 0);
    const float3 rgb = rgba.rgb;
    const float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));

    float v;
    switch (mode)
    {
    case 1: v = rgb.r; break;
    case 2: v = rgb.g; break;
    case 3: v = rgb.b; break;
    case 4:
        v = 1.0 - saturate(abs(luma - key_center) / max(key_range, 1.0e-4));
        break;
    case 5:
        v = 1.0 - saturate(distance(rgb, float3(key_r, key_g, key_b)) /
                           max(key_range, 1.0e-4));
        break;
    case 6: v = rgba.a; break;
    case 7:
        // Motion mask (spec §8): flow magnitude in px, 16 px = full white;
        // the levels below ARE the threshold window.
        v = saturate(length(rgb.rg) / 16.0);
        break;
    default: v = luma; break;
    }

    // Levels — users will live in these.
    v = saturate((v - black_point) / max(white_point - black_point, 1.0e-4));
    v = pow(v, 1.0 / max(gamma, 1.0e-3));
    if (invert != 0)
        v = 1.0 - v;
    output[tid.xy] = float4(v, v, v, 1.0);
}
