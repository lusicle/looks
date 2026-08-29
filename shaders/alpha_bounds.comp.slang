// The CPU must clear the bounds image to 0xFFFFFFFF before this dispatch.
// Cells 2 and 3 hold the complement of max. The reader must decode them.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] Sampler2D source;
[[vk::binding(1, 0)]] RWTexture2D<uint> bounds;

groupshared uint g_min_x;
groupshared uint g_min_y;
groupshared uint g_max_xc;
groupshared uint g_max_yc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    if (gtid.x == 0 && gtid.y == 0)
    {
        g_min_x = 0xFFFFFFFFu;
        g_min_y = 0xFFFFFFFFu;
        g_max_xc = 0xFFFFFFFFu;
        g_max_yc = 0xFFFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();

    bool covered = tid.x < width && tid.y < height;
    if (covered)
    {
        const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
        covered = source.SampleLevel(uv, 0).a >= 0.004;
    }
    if (covered)
    {
        InterlockedMin(g_min_x, tid.x);
        InterlockedMin(g_min_y, tid.y);
        InterlockedMin(g_max_xc, 0xFFFFFFFFu - tid.x);
        InterlockedMin(g_max_yc, 0xFFFFFFFFu - tid.y);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gtid.x == 0 && gtid.y == 0 && g_min_x != 0xFFFFFFFFu)
    {
        InterlockedMin(bounds[uint2(0, 0)], g_min_x);
        InterlockedMin(bounds[uint2(1, 0)], g_min_y);
        InterlockedMin(bounds[uint2(2, 0)], g_max_xc);
        InterlockedMin(bounds[uint2(3, 0)], g_max_yc);
    }
}
