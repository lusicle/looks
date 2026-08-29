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
    float preset;
    float push;
    float punch;
    float fade;
    float highlight;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const uint kPresets = 17;

// Rows are output R/G/B as weights over input RGB.
static const float3x3 kMatrix[kPresets] = {
    // neutral
    float3x3(1.00, 0.00, 0.00, 0.00, 1.00, 0.00, 0.00, 0.00, 1.00),
    // kodachrome
    float3x3(1.12, -0.06, -0.02, -0.04, 1.04, -0.02, -0.06, -0.02, 1.02),
    // portra
    float3x3(1.04, 0.02, -0.02, 0.00, 1.00, 0.00, -0.02, 0.02, 0.96),
    // vision3
    float3x3(1.02, 0.00, -0.01, 0.00, 1.01, 0.00, 0.00, 0.01, 1.00),
    // ektachrome
    float3x3(1.02, -0.02, 0.00, -0.02, 1.02, 0.00, 0.00, 0.02, 1.06),
    // fuji
    float3x3(0.98, 0.02, 0.00, 0.00, 1.06, -0.02, 0.00, 0.04, 1.00),
    // bleach bypass
    float3x3(1.00, 0.00, 0.00, 0.00, 1.00, 0.00, 0.00, 0.00, 1.00),
    // cross-process
    float3x3(1.10, -0.12, 0.02, -0.02, 1.08, -0.06, 0.06, 0.10, 0.84),
    // technicolor 2-strip
    float3x3(1.20, -0.10, -0.10, -0.10, 0.90, 0.20, -0.10, 0.20, 0.90),
    // ccd video
    float3x3(1.06, -0.02, -0.04, -0.02, 1.02, 0.00, -0.04, -0.02, 1.06),
    // wb failure warm
    float3x3(1.25, 0.05, -0.10, 0.02, 1.02, -0.04, -0.12, -0.06, 0.72),
    // wb failure cool
    float3x3(0.80, 0.00, 0.10, 0.00, 0.98, 0.04, 0.08, 0.04, 1.28),
    // technicolor 3-strip
    float3x3(1.35, -0.20, -0.15, -0.15, 1.25, -0.10, -0.15, -0.20, 1.35),
    // expired stock
    float3x3(0.92, 0.10, -0.02, 0.06, 1.04, 0.04, 0.02, 0.14, 0.78),
    // aerochrome IR
    float3x3(0.10, 1.20, -0.10, 0.80, 0.10, 0.05, 0.10, -0.10, 0.90),
    // b&w infrared
    float3x3(0.40, 0.90, -0.30, 0.40, 0.90, -0.30, 0.40, 0.90, -0.30),
    // instant film
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
    float3(-0.10, 0.80, 0.6),  // expired
    float3(0.20, 1.35, 0.0),   // aerochrome
    float3(0.15, 0.00, 0.0),   // b&w infrared
    float3(-0.05, 0.90, 0.35), // instant
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

    float3 lin = base.rgb * exp2(push);
    lin = max(mul(kMatrix[p], lin), 0.0);
    // A positive highlight deepens the shoulder. A negative one clips white.
    const float knee = 0.8 - saturate(highlight) * 0.45;
    const float soft = 1.6 + saturate(highlight) * 6.0;
    const float3 over = max(lin - knee, 0.0);
    lin = min(lin, knee) + over / (1.0 + over * soft);
    if (highlight < 0.0)
        lin = min(lin, 1.0 + highlight * 0.7);

    float3 enc = fx_srgb_oetf(saturate(lin));
    const float contrast = kTone[p].x + saturate(punch) * 0.6;
    enc = saturate((enc - 0.42) * (1.0 + contrast) + 0.42);
    const float l = fx_luma(enc);
    enc = saturate(lerp(float3(l, l, l), enc, kTone[p].y));
    // Presets 13 and 16 add a base fog to the fade knob.
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
