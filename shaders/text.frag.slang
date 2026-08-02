[[vk::push_constant]]
cbuffer Push
{
    float4x4 ortho;       // vertex stage, offset 0..64
    uint mode;            // fragment stage, offset 64
    float unit_range_x;   // atlas px_range / atlas width
    float unit_range_y;   // atlas px_range / atlas height
};

[[vk::binding(0, 0)]]
Sampler2D atlas;

struct FSInput
{
    [[vk::location(0)]] float2 v_uv    : TEXCOORD0;
    [[vk::location(1)]] float4 v_color : COLOR0;
};

[shader("fragment")]
float4 main(FSInput input) : SV_Target
{
    if (mode == 2) {
        const float4 s = atlas.Sample(input.v_uv);
        return float4(s.rgb * input.v_color.rgb, s.a * input.v_color.a);
    }
    float coverage;
    if (mode == 1) {
        // The screen-pixel range must come from the UV derivative, which
        // is linear across the quad, NOT from fwidth of the decoded
        // median: that is discontinuous where the channel ordering
        // switches and reads as ragged hard edges.
        const float3 s = atlas.Sample(input.v_uv).rgb;
        const float sd =
            max(min(s.r, s.g), min(max(s.r, s.g), s.b)) - 0.5;
        const float2 screen_tex =
            1.0 / max(fwidth(input.v_uv), float2(1.0e-6, 1.0e-6));
        const float px_raw =
            0.5 * (unit_range_x * screen_tex.x +
                   unit_range_y * screen_tex.y);
        // Minified glyphs, where px_raw < 1, cannot be antialiased by
        // widening the band: a floor of 1 lets it cover the ENTIRE field
        // and reads as a glow around every glyph. Bound the band to half
        // the field and melt sub-pixel text toward transparent instead.
        const float px_range = max(px_raw, 2.0);
        const float embolden = 0.2 * saturate(px_raw - 0.75);
        coverage = saturate(sd * px_range + 0.5 + embolden);
        coverage *= saturate(0.35 + 0.65 * px_raw);
    } else {
        coverage = atlas.Sample(input.v_uv).r;
    }
    return float4(input.v_color.rgb, input.v_color.a * coverage);
}
