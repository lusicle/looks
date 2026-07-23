// Voronoi / Delaunay shatter (spec §6.6): the frame breaks into cellular
// shards. Seed points live on a jittered grid (seeded, slowly drifting);
// every pixel finds its nearest seed, the whole shard displaces by a
// per-cell offset, and shard borders darken like cracks.
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
    float cells;     // grid cells across the width
    float shatter;   // per-cell displacement, px
    float edge;      // crack darkening
    float drift;     // seed-point wander, hz
    float style;     // 0 voronoi cells, 1 delaunay-style triangle facets
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

    const float cell_px = size.x / clamp(cells, 4.0, 256.0);
    const float2 p = (float2(tid.xy) + 0.5) / cell_px;
    const int2 pc = int2(floor(p));
    const float t = (fps > 0.0 ? float(frame) / fps : 0.0) * drift;

    // Three nearest jittered seeds over the 3x3 neighborhood (the third
    // feeds the Delaunay-style facets).
    float best = 1.0e9, second = 1.0e9, third = 1.0e9;
    uint best_hash = 0u;
    float2 sp1 = 0.0, sp2 = 0.0, sp3 = 0.0;
    [unroll]
    for (int j = -1; j <= 1; ++j) {
        [unroll]
        for (int i = -1; i <= 1; ++i) {
            const int2 c = pc + int2(i, j);
            const uint cx = uint(c.x + 4096);
            const uint cy = uint(c.y + 4096);
            const uint h = fx_hash3(seed, cx, cy);
            float2 jitter = float2(fx_hash01(h),
                                   fx_hash01(fx_hash(h ^ 0x777u)));
            if (drift > 0.001) {
                jitter.x = 0.5 + (jitter.x - 0.5) *
                           (0.6 + 0.8 * fx_value_noise1(h, t + 3.1));
                jitter.y = 0.5 + (jitter.y - 0.5) *
                           (0.6 + 0.8 * fx_value_noise1(h ^ 0x55u, t));
            }
            const float2 sp = float2(c) + jitter;
            const float2 d = p - sp;
            const float dist = dot(d, d);
            if (dist < best) {
                third = second;
                sp3 = sp2;
                second = best;
                sp2 = sp1;
                best = dist;
                sp1 = sp;
                best_hash = h;
            } else if (dist < second) {
                third = second;
                sp3 = sp2;
                second = dist;
                sp2 = sp;
            } else if (dist < third) {
                third = dist;
                sp3 = sp;
            }
        }
    }

    // Shard displacement: the whole cell samples from one offset.
    const float2 offset =
        (float2(fx_hash01(fx_hash(best_hash ^ 0x123u)),
                fx_hash01(fx_hash(best_hash ^ 0x456u))) - 0.5) *
        2.0 * shatter;
    const float border = sqrt(second) - sqrt(best);
    float3 col;
    if (style >= 0.5) {
        // Delaunay-style facets (spec §6.6): flat-shade each triangle of
        // three mutually nearest seeds from its centroid; ink the
        // bisector edges as the wireframe.
        const float2 centroid_p = (sp1 + sp2 + sp3) / 3.0;
        const float2 cuv = clamp(
            centroid_p * cell_px / size + offset / size, 0.0, 1.0);
        col = input.SampleLevel(cuv, 0).rgb;
        const float wire = 1.0 - smoothstep(0.0, 0.08, border);
        col *= 1.0 - wire * edge;
    } else {
        const float2 suv = clamp(uv + offset / size, 0.0, 1.0);
        col = input.SampleLevel(suv, 0).rgb;
        // Crack lines where the two nearest seeds are equidistant.
        const float crack = 1.0 - smoothstep(0.0, 0.12, border);
        col *= 1.0 - crack * edge;
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
