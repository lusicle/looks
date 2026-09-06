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
    float canvas_w;
    float canvas_h;
    uint history_valid;
    float dust;
    float scratch;
    float size;
    float hair;
    float tex_amt;
    float boil;      // reseed rate, hz
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D plate;   // dark = damage
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
    const float2 material_px = uv * float2(canvas_w, canvas_h);
    const float pixel_width = canvas_w / float(width);

    float dark = 0.0, light = 0.0;

    // bt gates all animated terms. boil = 0 must freeze every one.
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float bt = boil > 0.0 ? t : 0.0;
    const uint dust_tick = uint(bt * boil);
    const uint scratch_epoch = uint(bt * boil / 12.0);
    const uint hair_epoch = uint(bt * boil / 22.0);

    const float cell_px = 24.0 * size;
    const uint2 cell = uint2(material_px / cell_px);
    const uint h = fx_hash4(seed, dust_tick, cell.x, cell.y);
    if (fx_hash01(h) < dust * 0.35) {
        const float2 cpos = (float2(cell) + 0.11 +
                             float2(fx_hash01(fx_hash(h ^ 0x11u)),
                                    fx_hash01(fx_hash(h ^ 0x22u))) * 0.78) * cell_px;
        const float2 d = (material_px - cpos) / (cell_px * 0.5);
        const float rx = 0.05 + 0.16 * fx_hash01(fx_hash(h ^ 0x33u));
        const float ry = 0.05 + 0.16 * fx_hash01(fx_hash(h ^ 0x44u));
        const float e = (d.x * d.x) / (rx * rx) + (d.y * d.y) / (ry * ry);
        const float shape = 1.0 - smoothstep(0.35, 1.0, e);
        if (shape > 0.0) {
            const float dens =
                0.25 + 0.75 * fx_hash01(fx_hash(h ^ 0x66u));
            if (fx_hash01(fx_hash(h ^ 0x55u)) < 0.8)
                dark = shape * dens;
            else
                light = shape * dens;
        }
    }

    const uint lane = uint(material_px.x / 48.0);
    const uint sh = fx_hash4(seed ^ 0xABCDu, scratch_epoch, lane, 0u);
    if (fx_hash01(sh) < scratch * 0.4) {
        const float wobble =
            fx_value_noise1(sh, material_px.y * 0.01 + bt * 7.0) - 0.5;
        const float sx = (float(lane) + 0.5) * 48.0 + wobble * 20.0;
        const float dist = abs(material_px.x - sx);
        const float w = size * (0.5 + fx_hash01(fx_hash(sh ^ 0x77u)));
        const float footprint = canvas_w / float(width);
        if (dist < w + footprint) {
            const float fade = (1.0 - smoothstep(max(w - footprint * 0.5, 0.0), w + footprint * 0.5, dist)) * min(1.0, w / footprint);
            const float dens =
                0.4 + 0.6 * fx_hash01(fx_hash(sh ^ 0x99u));
            if (fx_hash01(fx_hash(sh ^ 0x88u)) < 0.5)
                light = max(light, fade * dens * 0.8);
            else
                dark = max(dark, fade * dens * 0.7);
        }
    }

    if (tex_amt > 0.001) {
        const float2 tex_off =
            float2(fx_hash01(fx_hash3(seed, scratch_epoch, 21u)),
                   fx_hash01(fx_hash3(seed, scratch_epoch, 22u)));
        const float2 tuv =
            frac(material_px / (256.0 * size) + tex_off);
        const float tv = plate.SampleLevel(tuv, 0).r;
        dark = max(dark, saturate(1.0 - tv) * tex_amt);
    }

    if (hair > 0.001) {
        const uint nhair = uint(hair * 3.0 + 0.999);
        for (uint hi = 0u; hi < 3u; ++hi) {
            if (hi >= nhair) break;
            const uint hh = fx_hash4(seed ^ 0x4A17u, hair_epoch, hi, 0u);
            if (fx_hash01(hh) > hair) continue;
            const uint edge = fx_hash(hh ^ 0x99u) % 4u;
            const float along = fx_hash01(fx_hash(hh ^ 0xAAu));
            float2 a;
            if (edge == 0u) a = float2(along, 0.0);
            else if (edge == 1u) a = float2(along, 1.0);
            else if (edge == 2u) a = float2(0.0, along);
            else a = float2(1.0, along);
            const float2 inward = float2(0.5, 0.5) - a;
            const float reach =
                0.10 + 0.18 * fx_hash01(fx_hash(hh ^ 0xBBu));
            const float2 b = a + inward * reach * 2.0;
            const float2 side = float2(-inward.y, inward.x);
            const float bow =
                (fx_hash01(fx_hash(hh ^ 0xCCu)) - 0.5) * 0.25;
            const float tremble =
                (fx_value_noise1(hh, bt * 6.0) - 0.5) * 0.02;
            const float2 ctrl =
                (a + b) * 0.5 + side * (bow + tremble);
            float best = 1.0e9;
            float2 last = a * float2(canvas_w, canvas_h);
            [unroll]
            for (int s = 1; s <= 12; ++s) {
                const float bt2 = float(s) / 12.0;
                const float2 p = lerp(lerp(a, ctrl, bt2),
                                      lerp(ctrl, b, bt2), bt2);
                const float2 next = p * float2(canvas_w, canvas_h);
                best = min(best, fx_segment_distance(material_px, last, next));
                last = next;
            }
            const float wpx = size * 1.1;
            const float coverage = 1.0 - smoothstep(max(0.0, wpx - pixel_width * 0.5),
                                                   wpx + pixel_width * 0.5, best);
            dark = max(dark, coverage * min(1.0, wpx / pixel_width) * 0.8);
        }
    }

    const float bright = saturate(light) * 0.7;
    const float3 stained = base.rgb * (1.0 - saturate(dark) * 0.85);
    const float4 result = float4(stained * (1.0 - bright) + bright,
                                base.a + bright * (1.0 - base.a));
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
