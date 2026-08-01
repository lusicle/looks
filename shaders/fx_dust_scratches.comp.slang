// Dust / scratches / hair: procedural damage. Default is a
// FIXED PLATE — every mark seed-locked in place, the damage of one
// photographed plate or print. `boil` > 0 switches to projected-print
// behavior: dust re-rolls at that rate, scratches persist ~12 ticks and
// wobble, gate hairs cling for ~22 ticks and tremble (24 hz = classic
// per-frame film-dirt flicker). Encoded space (damage lives on the print).
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
    float dust;      // fleck density
    float scratch;   // scratch density
    float size;      // damage scale
    float hair;      // gate-hair density
    float tex_amt;   // texture-driven damage from the plate
    float boil;      // 0 = fixed plate, else reseed rate (hz)
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D plate;   // damage plate: dark = damage
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size_px = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size_px;
    const float4 base = input.SampleLevel(uv, 0);

    float3 enc = fx_srgb_oetf(saturate(base.rgb));
    float dark = 0.0, light = 0.0;

    // One clock for everything: frozen at boil = 0 (fixed plate), so no
    // mark, wobble, or tremble ever moves.
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float bt = boil > 0.0 ? t : 0.0;          // animated time terms
    const uint dust_tick = uint(bt * boil);
    const uint scratch_epoch = uint(bt * boil / 12.0);
    const uint hair_epoch = uint(bt * boil / 22.0);

    // Dust: 24px cells (scaled), a fresh sparse fleck set per tick.
    const float cell_px = 24.0 * size;
    const uint2 cell = uint2(float2(tid.xy) / cell_px);
    const uint h = fx_hash4(seed, dust_tick, cell.x, cell.y);
    if (fx_hash01(h) < dust * 0.35) {
        // Fleck: an irregular soft-edged ellipse somewhere inside the
        // cell, each with its own density — most are faint smudges, a
        // few land dark.
        const float2 cpos = (float2(cell) +
                             float2(fx_hash01(fx_hash(h ^ 0x11u)),
                                    fx_hash01(fx_hash(h ^ 0x22u)))) * cell_px;
        const float2 d = (float2(tid.xy) - cpos) / (cell_px * 0.5);
        const float rx = 0.05 + 0.16 * fx_hash01(fx_hash(h ^ 0x33u));
        const float ry = 0.05 + 0.16 * fx_hash01(fx_hash(h ^ 0x44u));
        const float e = (d.x * d.x) / (rx * rx) + (d.y * d.y) / (ry * ry);
        const float shape = 1.0 - smoothstep(0.35, 1.0, e);
        if (shape > 0.0) {
            const float dens =
                0.25 + 0.75 * fx_hash01(fx_hash(h ^ 0x66u));
            // Mostly dark flecks; occasionally a bright one (emulsion gap).
            if (fx_hash01(fx_hash(h ^ 0x55u)) < 0.8)
                dark = shape * dens;
            else
                light = shape * dens;
        }
    }

    // Scratches: candidate columns (48px lanes); boiling prints wobble
    // them within the lane, a fixed plate holds them dead still.
    const uint lane = tid.x / 48u;
    const uint sh = fx_hash4(seed ^ 0xABCDu, scratch_epoch, lane, 0u);
    if (fx_hash01(sh) < scratch * 0.4) {
        const float wobble =
            fx_value_noise1(sh, float(tid.y) * 0.01 + bt * 7.0) - 0.5;
        const float sx = (float(lane) + 0.5) * 48.0 + wobble * 20.0;
        const float dist = abs(float(tid.x) - sx);
        const float w = size * (0.5 + fx_hash01(fx_hash(sh ^ 0x77u)));
        if (dist < w) {
            const float fade = 1.0 - dist / w;
            const float dens =
                0.4 + 0.6 * fx_hash01(fx_hash(sh ^ 0x99u));
            if (fx_hash01(fx_hash(sh ^ 0x88u)) < 0.5)
                light = max(light, fade * dens * 0.8);
            else
                dark = max(dark, fade * dens * 0.7);
        }
    }

    // Texture-driven damage: the plate tiled over the frame,
    // re-offset per scratch epoch when boiling; parked when fixed. Dark
    // plate marks become dark print damage.
    if (tex_amt > 0.001) {
        const float2 tex_off =
            float2(fx_hash01(fx_hash3(seed, scratch_epoch, 21u)),
                   fx_hash01(fx_hash3(seed, scratch_epoch, 22u)));
        const float2 tuv =
            frac(float2(tid.xy) / (256.0 * size) + tex_off);
        const float tv = plate.SampleLevel(tuv, 0).r;
        dark = max(dark, saturate(1.0 - tv) * tex_amt);
    }

    // Gate hairs: a few curved fibers anchored at the frame
    // edge. Boiling prints re-seed them per hair epoch and tremble in
    // between; a fixed plate freezes them. Each is a quadratic arc;
    // distance is checked against sampled arc points.
    if (hair > 0.001) {
        const uint nhair = uint(hair * 3.0 + 0.999);
        for (uint hi = 0u; hi < 3u; ++hi) {
            if (hi >= nhair) break;
            const uint hh = fx_hash4(seed ^ 0x4A17u, hair_epoch, hi, 0u);
            if (fx_hash01(hh) > hair) continue;
            // Anchor on one of the four edges; the arc bows inward.
            const uint edge = fx_hash(hh ^ 0x99u) % 4u;
            const float along = fx_hash01(fx_hash(hh ^ 0xAAu));
            float2 a;   // root on the edge
            if (edge == 0u) a = float2(along, 0.0);
            else if (edge == 1u) a = float2(along, 1.0);
            else if (edge == 2u) a = float2(0.0, along);
            else a = float2(1.0, along);
            const float2 inward = float2(0.5, 0.5) - a;
            const float reach =
                0.10 + 0.18 * fx_hash01(fx_hash(hh ^ 0xBBu));
            const float2 b = a + inward * reach * 2.0;   // tip
            const float2 side = float2(-inward.y, inward.x);
            const float bow =
                (fx_hash01(fx_hash(hh ^ 0xCCu)) - 0.5) * 0.25;
            const float tremble =
                (fx_value_noise1(hh, bt * 6.0) - 0.5) * 0.02;
            const float2 ctrl =
                (a + b) * 0.5 + side * (bow + tremble);
            // March the quadratic bezier and keep the closest approach.
            float best = 1.0e9;
            [unroll]
            for (int s = 0; s <= 12; ++s) {
                const float bt2 = float(s) / 12.0;
                const float2 p = lerp(lerp(a, ctrl, bt2),
                                      lerp(ctrl, b, bt2), bt2);
                const float2 dpx = (uv - p) * size_px;
                best = min(best, dot(dpx, dpx));
            }
            const float wpx = size * 1.1;
            const float d = sqrt(best);
            if (d < wpx)
                dark = max(dark, (1.0 - d / wpx) * 0.8);
        }
    }

    // Composite as artifacts IN the image, not stamps on top: dirt blocks
    // light (multiply — vanishes into the shadows), emulsion gaps flash
    // (screen — vanishes into the highlights). Both inherit the image's
    // own tonality instead of painting flat gray.
    enc = enc * (1.0 - saturate(dark) * 0.85);
    enc = enc + saturate(light) * 0.7 * (1.0 - enc);
    enc = saturate(enc);

    const float4 result = float4(fx_srgb_eotf(enc), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
