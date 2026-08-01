// ColorScience: film-stock color via per-preset 3x3 matrix +
// tone shaping. Pipeline: linear -> exposure (push/pull) -> stock matrix ->
// highlight rolloff -> encoded-domain curve (contrast punch around middle
// gray, fade lifts blacks, saturation) -> back to linear.
//
// Presets: 0 neutral, 1 kodachrome, 2 portra, 3 vision3, 4 ektachrome,
// 5 fuji, 6 bleach bypass, 7 cross-process, 8 technicolor 2-strip,
// 9 ccd video, 10 wb fail warm, 11 wb fail cool, 12 technicolor 3-strip,
// 13 expired stock, 14 aerochrome IR, 15 b&w infrared, 16 instant film.
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
    float preset;      // stock selector
    float push;        // stops of push/pull
    float punch;       // extra contrast 0..1
    float fade;        // black lift 0..1
    float highlight;   // -1 hard clip .. 0 neutral .. +1 deep rolloff
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const uint kPresets = 17;

// Rows are output R/G/B as weights over input RGB.
static const float3x3 kMatrix[kPresets] = {
    // neutral
    float3x3(1.00, 0.00, 0.00, 0.00, 1.00, 0.00, 0.00, 0.00, 1.00),
    // kodachrome: punchy warm reds, deep blue-cyan shadows
    float3x3(1.12, -0.06, -0.02, -0.04, 1.04, -0.02, -0.06, -0.02, 1.02),
    // portra: gentle, warm, protected skin band
    float3x3(1.04, 0.02, -0.02, 0.00, 1.00, 0.00, -0.02, 0.02, 0.96),
    // vision3: near-neutral negative, mild teal shadows
    float3x3(1.02, 0.00, -0.01, 0.00, 1.01, 0.00, 0.00, 0.01, 1.00),
    // ektachrome: cool clean punch
    float3x3(1.02, -0.02, 0.00, -0.02, 1.02, 0.00, 0.00, 0.02, 1.06),
    // fuji: green-teal bias
    float3x3(0.98, 0.02, 0.00, 0.00, 1.06, -0.02, 0.00, 0.04, 1.00),
    // bleach bypass: silver retained — heavy desat handled below
    float3x3(1.00, 0.00, 0.00, 0.00, 1.00, 0.00, 0.00, 0.00, 1.00),
    // cross-process: twisted channels
    float3x3(1.10, -0.12, 0.02, -0.02, 1.08, -0.06, 0.06, 0.10, 0.84),
    // technicolor 2-strip: red/cyan only
    float3x3(1.20, -0.10, -0.10, -0.10, 0.90, 0.20, -0.10, 0.20, 0.90),
    // ccd video: slight magenta highlights, clipped blue
    float3x3(1.06, -0.02, -0.04, -0.02, 1.02, 0.00, -0.04, -0.02, 1.06),
    // wb failure warm (tungsten left on)
    float3x3(1.25, 0.05, -0.10, 0.02, 1.02, -0.04, -0.12, -0.06, 0.72),
    // wb failure cool (daylight indoors)
    float3x3(0.80, 0.00, 0.10, 0.00, 0.98, 0.04, 0.08, 0.04, 1.28),
    // technicolor 3-strip: aggressive dye separation — saturated
    // primaries, hard channel isolation
    float3x3(1.35, -0.20, -0.15, -0.15, 1.25, -0.10, -0.15, -0.20, 1.35),
    // expired stock: green-cyan fog, magenta-shifted mids, weak blues
    float3x3(0.92, 0.10, -0.02, 0.06, 1.04, 0.04, 0.02, 0.14, 0.78),
    // aerochrome IR false color: chlorophyll (green) lands in red —
    // foliage burns magenta-red, skies stay cyan-blue
    float3x3(0.10, 1.20, -0.10, 0.80, 0.10, 0.05, 0.10, -0.10, 0.90),
    // b&w infrared: identical rows = mono; green-heavy, blue-negative
    // weights glow the foliage and blacken the skies
    float3x3(0.40, 0.90, -0.30, 0.40, 0.90, -0.30, 0.40, 0.90, -0.30),
    // instant film: cool green-cyan shadows, starved blue, creamy flat
    float3x3(1.02, 0.04, -0.02, 0.02, 1.04, 0.00, -0.04, 0.06, 0.88),
};

// {extra_contrast, saturation, warmth_of_fade}
static const float3 kTone[kPresets] = {
    float3(0.00, 1.00, 0.0),   // neutral
    float3(0.25, 1.25, 0.1),   // kodachrome
    float3(0.05, 0.92, 0.2),   // portra
    float3(0.02, 0.98, 0.0),   // vision3
    float3(0.20, 1.15, 0.0),   // ektachrome
    float3(0.10, 1.08, 0.0),   // fuji
    float3(0.45, 0.35, 0.0),   // bleach bypass
    float3(0.30, 1.30, 0.3),   // cross-process
    float3(0.25, 1.20, 0.1),   // technicolor
    float3(0.15, 1.10, 0.0),   // ccd
    float3(0.05, 0.95, 0.4),   // wb warm
    float3(0.05, 0.95, 0.0),   // wb cool
    float3(0.35, 1.45, 0.0),   // technicolor 3-strip
    float3(-0.10, 0.80, 0.6),  // expired: flat, desaturated, warm fog
    float3(0.20, 1.35, 0.0),   // aerochrome: punchy false color
    float3(0.15, 0.00, 0.0),   // b&w infrared
    float3(-0.05, 0.90, 0.35), // instant: flat, slightly warm fade
};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const uint p = min(uint(preset + 0.5), kPresets - 1);

    // Linear-domain: exposure, stock matrix, highlight rolloff.
    float3 lin = base.rgb * exp2(push);
    lin = max(mul(kMatrix[p], lin), 0.0);
    // Highlight clip/rolloff: positive pulls the shoulder
    // down into a deep filmic rolloff; negative hard-clips to a sinking
    // white point — the blown-video look. 0 keeps the stock shoulder.
    const float knee = 0.8 - saturate(highlight) * 0.45;
    const float soft = 1.6 + saturate(highlight) * 6.0;
    const float3 over = max(lin - knee, 0.0);
    lin = min(lin, knee) + over / (1.0 + over * soft);
    if (highlight < 0.0)
        lin = min(lin, 1.0 + highlight * 0.7);

    // Encoded-domain tone shaping.
    float3 enc = fx_srgb_oetf(saturate(lin));
    const float contrast = kTone[p].x + saturate(punch) * 0.6;
    enc = saturate((enc - 0.42) * (1.0 + contrast) + 0.42);
    const float l = fx_luma(enc);
    enc = saturate(lerp(float3(l, l, l), enc, kTone[p].y));
    // Fade: lifted blacks, optionally warmed. The expired and instant
    // presets carry a baked-in base fog on top of the knob.
    const float lift =
        saturate(fade + (p == 13u ? 0.45 : (p == 16u ? 0.22 : 0.0))) *
        0.14;
    const float3 fade_tint =
        float3(1.0 + kTone[p].z * 0.4, 1.0, 1.0 - kTone[p].z * 0.2);
    enc = enc * (1.0 - lift) + lift * fade_tint * 0.6;

    const float4 result = float4(fx_srgb_eotf(saturate(enc)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
