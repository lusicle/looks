// Atlas tiles MUST be ordered by ink coverage, dark to light: the tile
// index is a direct luma lookup, with no indirection table.

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
    float cell;        // px
    float set_index;   // engine picks the atlas, unread here
    float mode;        // 0 mono, 1 colored glyphs, 2 glyph * pixel
    float jitter;
    // Engine-appended atlas grid.
    uint atlas_cols;
    uint atlas_rows;
    float tile_px;
    uint atlas_color;   // 1 = RGBA tiles: alpha coverage, own hue
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D atlas;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float cell_px = max(cell, 2.0);
    const uint2 cell_id = uint2(float2(tid.xy) / cell_px);
    const float2 cell_center = (float2(cell_id) + 0.5) * cell_px / size;
    const float3 cell_rgb =
        input.SampleLevel(saturate(cell_center), 0).rgb;
    // Encoded-domain luma, so the glyph ramp is perceptually even.
    float l = fx_luma(fx_srgb_oetf(saturate(cell_rgb)));

    if (jitter > 0.0)
    {
        const float j =
            fx_hash01(fx_hash3(seed, cell_id.x, cell_id.y)) - 0.5;
        l = saturate(l + j * jitter * 0.5);
    }

    const uint cols = max(atlas_cols, 1u);
    const uint rows = max(atlas_rows, 1u);
    const uint tiles = cols * rows;
    const float tile_size = max(tile_px, 1.0);
    const uint tile = min(uint(l * float(tiles - 1) + 0.5), tiles - 1);
    const uint2 tile_xy = uint2(tile % cols, tile / cols);
    const float2 in_cell = frac(float2(tid.xy) / cell_px);
    const uint2 texel = uint2(float2(tile_xy) * tile_size +
                              min(in_cell * tile_size, tile_size - 1.0));
    const float2 atlas_uv =
        (float2(texel) + 0.5) /
        float2(float(cols) * tile_size, float(rows) * tile_size);
    const float4 texel4 = atlas.SampleLevel(atlas_uv, 0);
    const float g = atlas_color != 0u ? texel4.a : texel4.r;
    const float3 glyph_rgb = atlas_color != 0u
        ? texel4.rgb * texel4.a
        : float3(g, g, g);

    float3 rgb;
    float out_a = base.a;
    const int m = int(mode + 0.5);
    if (m == 0) {
        rgb = glyph_rgb;
        out_a = max(base.a, g);   // drawn ink unions its coverage in
    } else if (m == 1) {
        rgb = atlas_color != 0u ? glyph_rgb * (0.35 + 0.65 * fx_luma(cell_rgb))
                                : cell_rgb * g;
        out_a = max(base.a, g);
    } else {
        rgb = base.rgb * g;   // blackout style dims color only
    }

    const float4 result = float4(rgb, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
