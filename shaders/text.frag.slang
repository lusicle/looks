// Text fragment shader. Three atlas flavors, selected by the push flag:
//  - 0 bitmap coverage (A8 sampled as R): the compiled-in debug font;
//  - 1 MSDF (RGBA, baked by fontbake via msdf-atlas-gen): median-of-RGB is
//    a signed distance around 0.5; the screen-space derivative keeps the
//    edge one pixel wide at any scale;
//  - 2 plain RGBA image (thumbnail strips etc.): sample × vertex tint.

[[vk::push_constant]]
cbuffer Push
{
    float4x4 ortho;       // vertex stage (offset 0..64)
    uint mode;            // fragment stage (offset 64)
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
        // Canonical msdfgen decode: the screen-pixel range comes from the
        // UV derivative (linear across the quad) scaled by the atlas's
        // distance range — NOT from fwidth of the decoded median, which
        // is discontinuous where the channel ordering switches and reads
        // as ragged hard edges. max(.., 1) keeps at least a one-pixel AA
        // band under minification; +0.2 is a constant-pixel embolden.
        const float3 s = atlas.Sample(input.v_uv).rgb;
        const float sd =
            max(min(s.r, s.g), min(max(s.r, s.g), s.b)) - 0.5;
        const float2 screen_tex =
            1.0 / max(fwidth(input.v_uv), float2(1.0e-6, 1.0e-6));
        const float px_raw =
            0.5 * (unit_range_x * screen_tex.x +
                   unit_range_y * screen_tex.y);
        // Minified glyphs (px_raw < 1: one screen pixel spans more than
        // the whole distance range) cannot be antialiased by widening the
        // band — a floor of 1 let the band cover the ENTIRE field, which
        // reads as a glow around every glyph at small zoom. Bound the
        // band to half the field, retire the embolden as it shrinks, and
        // melt sub-pixel text toward transparent instead.
        const float px_range = max(px_raw, 2.0);
        const float embolden = 0.2 * saturate(px_raw - 0.75);
        coverage = saturate(sd * px_range + 0.5 + embolden);
        coverage *= saturate(0.35 + 0.65 * px_raw);
    } else {
        coverage = atlas.Sample(input.v_uv).r;
    }
    return float4(input.v_color.rgb, input.v_color.a * coverage);
}
