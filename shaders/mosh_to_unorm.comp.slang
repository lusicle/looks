// GPU mosh: moshed-state planes (r32ui bytes) to the R8_UNORM upload
// images the shared ycbcr_to_rgb pass samples. One full-res dispatch;
// chroma writes ride the half-res guard.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint cw;
    uint chh;
};

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> mosh_y;
[[vk::binding(1, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> mosh_u;
[[vk::binding(2, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> mosh_v;
[[vk::binding(3, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> up_y;
[[vk::binding(4, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> up_u;
[[vk::binding(5, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> up_v;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < width && tid.y < height)
        up_y[tid.xy] = float(mosh_y[tid.xy]) / 255.0;
    if (tid.x < cw && tid.y < chh)
    {
        up_u[tid.xy] = float(mosh_u[tid.xy]) / 255.0;
        up_v[tid.xy] = float(mosh_v[tid.xy]) / 255.0;
    }
}
