// GPU mosh: motion-compensated prediction, one thread per pixel. Every
// thread derives its 16x16 macroblock's motion vector independently (a few
// multiplies - cheaper than sharing), gathers the clamped source texel
// from the reference planes, and writes the prediction. Chroma rides the
// same dispatch: threads inside the half-res rect also gather U and V with
// the halved vector, truncated toward zero like the CPU walk did.
// mangle = 0 is the encoder's view (raw flow only); 1 applies the field
// replacement and scale/rotate/random mangling (rotation arrives as
// cos/sin push values so no trig runs here).

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint cw;
    uint chh;
    uint blocks_w;
    uint blocks_h;
    uint mangle;
    uint mv_field;        // 0 flow, 1 pan, 2 zoom, 3 swirl
    float mv_scale;
    float rot_c;
    float rot_s;
    float mv_random;      // +/- px of seeded randomization
    float field_amt;      // px at the frame edge (signed)
    uint seed_mv;
    uint frame;
    uint has_flow;
};

[[vk::binding(0, 0)]] Sampler2D flow_field;   // per-16px-block MVs in .xy
[[vk::binding(1, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> ref_y;
[[vk::binding(2, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> ref_u;
[[vk::binding(3, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> ref_v;
[[vk::binding(4, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> pred_y;
[[vk::binding(5, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> pred_u;
[[vk::binding(6, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> pred_v;

uint mosh_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float mosh_hash01(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

// lround port: half away from zero, truncating int() casts.
int round_away(float v)
{
    return v >= 0.0 ? int(v + 0.5) : -int(-v + 0.5);
}

int2 block_mv(uint mbx, uint mby)
{
    float vx = 0.0;
    float vy = 0.0;
    if (has_flow != 0 && mbx < blocks_w && mby < blocks_h)
    {
        const float2 uv = (float2(mbx, mby) + 0.5) /
                          float2(blocks_w, blocks_h);
        const float2 f = flow_field.SampleLevel(uv, 0).xy;
        vx = f.x;
        vy = f.y;
    }
    if (mangle != 0)
    {
        if (mv_field != 0)
        {
            const float px = (float(mbx) + 0.5) / float(blocks_w) - 0.5;
            const float py = (float(mby) + 0.5) / float(blocks_h) - 0.5;
            if (mv_field == 1) { vx = field_amt; vy = 0.0; }
            else if (mv_field == 2) { vx = px * 2.0 * field_amt;
                                      vy = py * 2.0 * field_amt; }
            else { vx = -py * 2.0 * field_amt; vy = px * 2.0 * field_amt; }
        }
        const float rx = vx * rot_c - vy * rot_s;
        const float ry = vx * rot_s + vy * rot_c;
        vx = rx * mv_scale;
        vy = ry * mv_scale;
        if (mv_random > 0.0)
        {
            const uint hb = mosh_hash(seed_mv ^ mosh_hash(frame ^ mosh_hash(
                mby * 4096u + mbx)));
            vx += (mosh_hash01(hb) - 0.5) * 2.0 * mv_random;
            vy += (mosh_hash01(mosh_hash(hb)) - 0.5) * 2.0 * mv_random;
        }
    }
    return int2(round_away(vx), round_away(vy));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < width && tid.y < height)
    {
        const int2 mv = block_mv(tid.x >> 4, tid.y >> 4);
        const int2 src = clamp(int2(tid.xy) - mv,
                               int2(0, 0),
                               int2(int(width) - 1, int(height) - 1));
        pred_y[tid.xy] = ref_y[src];
    }
    if (tid.x < cw && tid.y < chh)
    {
        const int2 mv = block_mv(tid.x >> 3, tid.y >> 3);
        const int2 half_mv = int2(mv.x / 2, mv.y / 2);   // trunc, like ivx/2
        const int2 src = clamp(int2(tid.xy) - half_mv,
                               int2(0, 0),
                               int2(int(cw) - 1, int(chh) - 1));
        pred_u[tid.xy] = ref_u[src];
        pred_v[tid.xy] = ref_v[src];
    }
}
