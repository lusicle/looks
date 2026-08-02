// The display-side counterpart to the signal-path effects: Composite and
// Snow simulate the cable, this simulates the glass.

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
    float curvature;
    float scanline;
    float mask_amt;
    float triad;       // px
    float panel;       // 0 crt tube, 1 lcd screen-door
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

    const bool lcd = panel >= 0.5;
    // Outside the curved screen is tube black. LCD glass is flat.
    float2 c = uv * 2.0 - 1.0;
    const float k = lcd ? 0.0 : curvature * 0.22;
    c *= 1.0 + k * dot(c, c);
    const float2 suv = c * 0.5 + 0.5;
    const bool off_screen = any(suv < 0.0) || any(suv > 1.0);

    float3 col = float3(0.0, 0.0, 0.0);
    if (!off_screen) {
        col = input.SampleLevel(suv, 0).rgb;

        if (lcd) {
            // scanline drives the row-gap depth here, triad the cell size.
            const float tw = max(triad, 2.0);
            const float cx = frac(float(tid.x) / (tw * 3.0)) * 3.0;
            const uint stripe = uint(cx) % 3u;
            const float in_stripe = frac(cx);
            const float border =
                smoothstep(0.0, 0.15, in_stripe) *
                (1.0 - smoothstep(0.85, 1.0, in_stripe));
            float3 sub = float3(0.08, 0.08, 0.08);
            if (stripe == 0u) sub.r = 1.0;
            else if (stripe == 1u) sub.g = 1.0;
            else sub.b = 1.0;
            const float ry = frac(float(tid.y) / tw);
            const float row_gap =
                1.0 - saturate(scanline) * smoothstep(0.82, 0.95, ry);
            col *= lerp(float3(1.0, 1.0, 1.0),
                        sub * border * row_gap,
                        saturate(mask_amt));
            col *= 1.0 + saturate(mask_amt) * 0.9;
        } else {
            // Gaps ride the CURVED coordinate so they bend with the glass.
            const float row = suv.y * size.y;
            const float gap =
                0.5 + 0.5 * cos(row * 3.14159265);
            col *= 1.0 - scanline * 0.6 * gap;

            const float tw = max(triad, 2.0);
            const uint stripe = uint(floor(float(tid.x) / tw)) % 3u;
            float3 grille = float3(0.35, 0.35, 0.35);
            if (stripe == 0u) grille.r = 1.0;
            else if (stripe == 1u) grille.g = 1.0;
            else grille.b = 1.0;
            col *= lerp(float3(1.0, 1.0, 1.0), grille, saturate(mask_amt));
            // Gain back the light the mask ate.
            col *= 1.0 + saturate(mask_amt) * 0.5 + scanline * 0.25;

            const float corner =
                saturate(1.0 - dot(c, c) * 0.28 * curvature);
            col *= corner;
        }
    }

    const float4 result = float4(col, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
