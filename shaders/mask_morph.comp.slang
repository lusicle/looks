// Mask expand / contract (spec §8 post ops): separable morphological
// max (dilate, grow > 0) or min (erode, grow < 0) over a +-radius window.
// Two passes (H then V) give a square structuring element — right for
// grow/shrink on soft grayscale masks.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float grow_px;     // signed; the sign picks dilate vs erode
    uint vertical;     // 0 = horizontal pass, 1 = vertical pass
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 texel = 1.0 / float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) * texel;
    const float2 step = vertical != 0u ? float2(0.0, texel.y)
                                       : float2(texel.x, 0.0);
    const bool dilate = grow_px > 0.0;
    const int radius = clamp(int(abs(grow_px) + 0.5), 1, 64);

    float v = input.SampleLevel(uv, 0).r;
    for (int i = 1; i <= radius; ++i)
    {
        const float a = input.SampleLevel(uv - step * float(i), 0).r;
        const float b = input.SampleLevel(uv + step * float(i), 0).r;
        v = dilate ? max(v, max(a, b)) : min(v, min(a, b));
    }
    output[tid.xy] = float4(v, v, v, 1.0);
}
