// UI fragment shader. Two paths selected per-vertex by `shape`:
//
//   1. Pass-through (shape.x == 0 && shape.y == 0): solid rects, gradients,
//      triangles, lines — interpolated vertex color straight out.
//   2. SDF rounded rect (radius > 0 || stroke > 0):
//      shape = { radius, stroke, halfW, halfH } in physical px. Fill when
//      stroke == 0; outline ring of width `stroke` otherwise. Edge AA via
//      fwidth of the SDF — correct at any UI scale.
//
// Blend is straight alpha (src_alpha / one_minus_src_alpha), so coverage is
// folded into the returned alpha.

struct FSInput
{
    [[vk::location(0)]] float2 v_uv    : TEXCOORD0;
    [[vk::location(1)]] float4 v_color : COLOR0;
    [[vk::location(2)]] float4 v_shape : TEXCOORD1;
};

[shader("fragment")]
float4 main(FSInput input) : SV_Target
{
    const float radius = input.v_shape.x;
    const float stroke = input.v_shape.y;

    if (radius <= 0.0 && stroke <= 0.0) {
        return input.v_color;
    }

    // UV spans [0,1] across the quad; reconstruct the offset from center.
    const float2 half_size = input.v_shape.zw;
    const float2 local = (input.v_uv - 0.5) * (2.0 * half_size);
    const float2 q = abs(local) - half_size + radius;
    const float sdf = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;

    const float aa = fwidth(sdf) + 1.0e-4;
    float alpha;
    if (stroke > 0.0) {
        const float half_stroke = stroke * 0.5;
        const float ring = abs(sdf + half_stroke) - half_stroke;
        alpha = 1.0 - smoothstep(-aa, aa, ring);
    } else {
        alpha = 1.0 - smoothstep(-aa, aa, sdf);
    }
    return float4(input.v_color.rgb, input.v_color.a * alpha);
}
