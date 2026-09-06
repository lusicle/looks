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
    float cell;        // px
    float set_index;
    float mode;
    float jitter;
    // The engine appends these atlas grid words.
    uint atlas_cols;
    uint atlas_rows;
    float tile_px;
    uint atlas_color;   // 1 means RGBA tiles with alpha coverage
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D atlas;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

float disc_integral(float2 p)
{
    float2 q = min(abs(p) / 0.18, 1.0);
    float a = min(q.x, sqrt(max(1.0 - q.y * q.y, 0.0)));
    float area = a * q.y + 0.5 * (q.x * sqrt(max(1.0 - q.x * q.x, 0.0)) +
        asin(q.x) - a * sqrt(max(1.0 - a * a, 0.0)) - asin(a));
    return sign(p.x) * sign(p.y) * area * (0.18 * 0.18);
}

float3 glyph_cell(float2 cell_id, float2 lo, float2 hi, float2 footprint, float3 pixel)
{
    float2 canvas = float2(canvas_w, canvas_h);
    float cell_px = max(cell, 2.0);
    uint set = uint(set_index + 0.5);
    uint rows = set == 4u ? 3u : 4u;
    float4 samples[8];
    float4 mean = 0.0;
    for (uint i = 0u; i < 2u * rows; ++i) {
        float2 p = (float2(i & 1u, i >> 1u) + 0.5) / float2(2.0, float(rows));
        samples[i] = input.SampleLevel(clamp((cell_id + p) * cell_px / canvas, 0.0, 1.0), 0);
        mean += samples[i] / float(2u * rows);
    }
    float3 color = fx_straight(mean);
    float l = fx_luma(fx_srgb_oetf(saturate(color)));
    float noise = (fx_hash01(fx_hash3(seed, uint(cell_id.x), uint(cell_id.y))) - 0.5) * jitter * 0.5;
    bool teletext_color = set == 4u && mode >= 0.5 && mode < 1.5;
    if (teletext_color) {
        float3 encoded = fx_srgb_oetf(saturate(color));
        color = step(float3(0.5, 0.5, 0.5), encoded / max(max(encoded.r, encoded.g), max(encoded.b, 1e-5)));
    }
    float g = 0.0;
    float3 glyph = 0.0;
    if (set >= 3u) {
        for (uint i = 0u; i < 2u * rows; ++i) {
            float3 sample_color = saturate(fx_straight(samples[i]));
            float level = teletext_color ? dot(sample_color, color) / max(dot(color, color), 1.0) :
                fx_luma(fx_srgb_oetf(sample_color));
            float threshold = (float((i * 5u) % (2u * rows)) + 0.5) / float(2u * rows);
            if (level + noise <= threshold) continue;
            float2 p = float2(i & 1u, i >> 1u) / float2(2.0, float(rows));
            if (set == 3u) {
                float2 center = p + float2(0.25, 0.125);
                float2 a = (lo - center) * float2(1.0, 2.0);
                float2 b = (hi - center) * float2(1.0, 2.0);
                g += max(disc_integral(b) - disc_integral(float2(a.x, b.y)) -
                    disc_integral(float2(b.x, a.y)) + disc_integral(a), 0.0) /
                    (2.0 * footprint.x * footprint.y);
            } else {
                float2 covered = max(min(hi, p + float2(0.5, 1.0 / 3.0)) - max(lo, p), 0.0);
                g += covered.x * covered.y / (footprint.x * footprint.y);
            }
        }
        g = saturate(g);
    } else {
        uint cols = max(atlas_cols, 1u);
        uint rows = max(atlas_rows, 1u);
        float tile_size = max(tile_px, 1.0);
        uint tile = min(uint(saturate(l + noise) * float(cols * rows - 1u) + 0.5), cols * rows - 1u);
        float2 atlas_size = float2(cols, rows) * (tile_size + 1.0);
        float2 origin = float2(tile % cols, tile / cols) * (tile_size + 1.0) + 0.5;
        float2 a = (origin + lo * tile_size) / atlas_size;
        float2 b = (origin + hi * tile_size) / atlas_size;
        float4 texel = max(atlas.SampleLevel(b, 0) - atlas.SampleLevel(float2(a.x, b.y), 0) -
            atlas.SampleLevel(float2(b.x, a.y), 0) + atlas.SampleLevel(a, 0), 0.0) /
            (footprint.x * footprint.y * tile_size * tile_size);
        g = atlas_color != 0u ? texel.a : texel.r;
        glyph = atlas_color != 0u ? texel.rgb : float3(g, g, g);
    }
    return mode < 0.5 ? float3(g, g, g) :
           mode < 1.5 ? (atlas_color != 0u && set < 3u ? glyph : color * g) : pixel * g;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float4 base = input.SampleLevel(uv, 0);
    if (base.a <= 0.0 || wet <= 0.0 || opacity <= 0.0) { output[tid.xy] = base; return; }
    float2 footprint = float2(canvas_w, canvas_h) / float2(width, height) / max(cell, 2.0);
    float2 lo = float2(tid.xy) * footprint;
    float2 hi = float2(tid.xy + 1u) * footprint;
    int2 first = int2(floor(lo));
    int2 last = int2(ceil(hi)) - 1;
    float3 rgb = 0.0;
    for (int y = first.y; y <= last.y; ++y) {
        for (int x = first.x; x <= last.x; ++x) {
            float2 id = float2(x, y);
            rgb += glyph_cell(id, max(lo - id, 0.0), min(hi - id, 1.0), footprint, fx_straight(base));
        }
    }
    float4 result = float4(rgb * base.a, base.a);
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
