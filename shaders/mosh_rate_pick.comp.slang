// GPU mosh rate control, pass 3: the ladder decision. Reads the exact
// per-rung stream sizes accumulated by the probe/reduce passes and writes
// the chosen quality into qsel - the wire reads it from there, so rate
// control never touches the CPU. Rule matches the CPU box: first rung
// whose stream fits the budget, else the ladder's floor.

#include "mosh_common.slang"

[[vk::push_constant]]
cbuffer Push
{
    uint rung_count;
    uint budget;          // bytes
    uint header_bytes;    // 1 for intra streams (quality byte), 0 for P
    uint pad0;
    uint4 rungs_lo;
    uint4 rungs_hi;
};

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> rate_bits;
[[vk::binding(1, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> qsel;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x != 0 || tid.y != 0)
        return;
    uint chosen = mosh_rung_at(rungs_lo, rungs_hi, rung_count - 1);
    for (uint r = 0; r < rung_count; ++r)
    {
        const uint bytes = header_bytes + (rate_bits[int2(int(r), 0)] + 7) / 8;
        if (bytes <= budget)
        {
            chosen = mosh_rung_at(rungs_lo, rungs_hi, r);
            break;
        }
    }
    qsel[int2(0, 0)] = chosen;
}
