// Frame fetch: I420 planes -> linear RGB f16. BT.709 limited
// range, transfer decoded with the sRGB EOTF (close enough to BT.709 camera
// gamma for v1; a proper transfer selector rides in with ColorScience).

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] Sampler2D plane_y;
[[vk::binding(1, 0)]] Sampler2D plane_u;
[[vk::binding(2, 0)]] Sampler2D plane_v;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

float3 srgb_eotf(float3 encoded)
{
    const float3 hi = pow((encoded + 0.055) / 1.055, 2.4);
    const float3 lo = encoded / 12.92;
    return lerp(hi, lo, step(encoded, 0.04045));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);

    const float y = plane_y.SampleLevel(uv, 0).r;
    const float cb = plane_u.SampleLevel(uv, 0).r;
    const float cr = plane_v.SampleLevel(uv, 0).r;

    // Limited range -> full, then BT.709 matrix.
    const float yf = (y * 255.0 - 16.0) / 219.0;
    const float cbf = (cb * 255.0 - 128.0) / 224.0;
    const float crf = (cr * 255.0 - 128.0) / 224.0;

    float3 rgb;
    rgb.r = yf + 1.5748 * crf;
    rgb.g = yf - 0.1873 * cbf - 0.4681 * crf;
    rgb.b = yf + 1.8556 * cbf;
    rgb = saturate(rgb);

    output[tid.xy] = float4(srgb_eotf(rgb), 1.0);
}
