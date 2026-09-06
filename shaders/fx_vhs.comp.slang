// Work in encoded space. The tape records a signal, not a scene.

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
    float mode;
    float luma_res;      // lines
    float chroma_res;    // lines
    float ringing;
    float noise;
    float chroma_noise;
    float dropouts;
    float tbe;           // px
    float skew;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const float kLines = 243.0;   // active lines per field

float enc_luma(float2 p)
{
    return fx_luma(fx_encoded(input.SampleLevel(p, 0)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const int sp = int(mode + 0.5);
    const float bw_s = sp == 0 ? 1.0 : (sp == 1 ? 0.82 : 0.6);
    const float nz_s = sp == 0 ? 1.0 : (sp == 1 ? 1.6 : 2.4);
    const float do_s = sp == 0 ? 1.0 : (sp == 1 ? 1.5 : 2.5);

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint field = uint(t * 59.94);
    const float2 material_px = uv * float2(canvas_w, canvas_h);
    const float line_h = 1.0 / kLines;               // uv
    const uint vline = min(uint(uv.y * kLines), uint(kLines) - 1u);

    const float jr =
        fx_hash01(fx_hash4(seed, vline, field, 0x7B5u)) - 0.5;
    const float wander =
        fx_value_noise1(seed ^ 0x3D1u, t * 2.2) - 0.5;
    float dx_px = jr * 2.0 * tbe + wander * tbe * 0.8;
    const float flag_w =
        (fx_value_noise1(seed ^ 0x9C4u, t * 1.3) - 0.5) * 2.0;
    dx_px += skew * 26.0 * exp(-float(vline) / 14.0) * flag_w;
    const float sx = uv.x - dx_px / canvas_w;
    const float sy = uv.y;

    // The ringing overshoot is biased bright on purpose.
    const float lres = max(luma_res * bw_s, 40.0);
    const float pitch = size.y / lres;               // px per luma sample
    const float sig = pitch * 0.55;
    float y_lp = 0.0, y_wide = 0.0;
    float wl = 0.0, ww = 0.0;
    [unroll]
    for (int k = 0; k < 9; ++k) {
        const float o = (float(k) - 4.0) * sig * 0.7;
        const float yv = enc_luma(float2(sx + o / size.x, sy));
        const float g1 = exp(-0.5 * (o * o) / (sig * sig));
        const float g2 = exp(-0.125 * (o * o) / (sig * sig));
        y_lp += yv * g1;
        y_wide += yv * g2;
        wl += g1;
        ww += g2;
    }
    y_lp /= wl;
    y_wide /= ww;
    const float pre = y_lp - y_wide;
    float y_out = y_lp + ringing * (pre > 0.0 ? 2.4 : 1.2) * pre;

    const float cres = max(chroma_res * bw_s, 6.0);
    const float cpitch = size.y / cres;
    float2 cbcr = float2(0.0, 0.0);
    float cw = 0.0;
    [unroll]
    for (int k = 0; k < 15; ++k) {
        const float o = (float(k) - 7.0) * cpitch * (1.5 / 7.0);
        const float2 p = float2(sx + o / size.x, sy - line_h * 0.5);
        const float3 enc =
            fx_encoded(input.SampleLevel(p, 0));
        const float yv = fx_luma(enc);
        const float wgt = 1.0 - abs(float(k) - 7.0) / 8.0;
        cbcr += fx_rgb_to_cbcr709(enc, yv) * wgt;
        cw += wgt;
    }
    cbcr /= cw;
    const float cn = saturate(chroma_noise * nz_s);
    const float ang =
        (fx_hash01(fx_hash4(seed ^ 0x51Du, vline, field, 1u)) - 0.5) *
            cn * 0.7 +
        (fx_value_noise1(seed ^ 0x2E7u, t * 3.7) - 0.5) * cn * 0.25;
    const float ca = cos(ang), sa = sin(ang);
    cbcr = float2(cbcr.x * ca - cbcr.y * sa, cbcr.x * sa + cbcr.y * ca);
    const float cmag = sqrt(dot(cbcr, cbcr));
    const float2 cxy = float2(material_px.x / 9.0,
                              float(vline) * 1.71 + float(field) * 77.0);
    const float2 csnow =
        float2(fx_vnoise2(seed ^ 0x77Au, cxy) - 0.5,
               fx_vnoise2(seed ^ 0x1B3u, cxy + 53.0) - 0.5);
    cbcr += csnow * cn * (0.04 + 0.5 * cmag);

    // The luma noise is smeared horizontally, never white grain.
    const float nz = saturate(noise * nz_s);
    const float2 nxy = float2(material_px.x / 6.0,
                              float(vline) * 2.13 + float(field) * 131.0);
    const float streak = fx_vnoise2(seed ^ 0x4F9u, nxy) - 0.5;
    const float fine =
        (fx_hash01(fx_hash4(seed, uint(material_px.x), uint(material_px.y), field)) - 0.5) * min(1.0, float(width) / canvas_w);
    y_out += (streak * 0.75 + fine * 0.25) * nz * 0.5 *
             (0.35 + 0.65 * (1.0 - y_out));

    float drop = 0.0;
    const uint dh = fx_hash4(seed ^ 0xD07u, vline, field, 2u);
    if (fx_hash01(dh) < saturate(dropouts * do_s) * 0.06) {
        const float x0 = fx_hash01(fx_hash(dh ^ 0x11u)) * 0.9;
        const float seg = 0.03 + 0.25 * fx_hash01(fx_hash(dh ^ 0x22u));
        const float rel = (uv.x - x0) / seg;
        if (rel > 0.0 && rel < 1.0)
            drop = pow(1.0 - rel, 0.6);
    }
    if (drop > 0.0) {
        const float y_up = enc_luma(float2(sx, sy - line_h));
        y_out = lerp(y_out, max(y_up, 0.85 + 0.3 * drop), drop);
        cbcr *= 1.0 - drop;
    }

    // Recompose through the shared 709 anchors.
    const float rr = y_out + cbcr.y * kFxCr709;
    const float bb = y_out + cbcr.x * kFxCb709;
    const float gg =
        (y_out - kFxLumaW.r * rr - kFxLumaW.b * bb) / kFxLumaW.g;
    const float3 enc_out = saturate(float3(rr, gg, bb));

    const float src_a = input.SampleLevel(float2(sx, sy), 0).a;
    const float out_a = max(src_a, drop);
    const float4 result = fx_from_encoded(enc_out, out_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
