
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
    float cols;
    float rows_n;
    float spread;
    float mono;
    float osd;
    uint pass_tile;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D history;   // frame at age == pass_age
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

static const uint kRing = 16;   // must match the engine kSlitRing

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const float2 size = float2(width, height);
    const uint nc = uint(clamp(cols, 1.0, 4.0) + 0.5);
    const uint nr = uint(clamp(rows_n, 1.0, 4.0) + 0.5);
    const uint tx = pass_tile % nc;
    const uint ty = pass_tile / nc;
    const uint tile = pass_tile;
    const uint2 start = uint2(tx * width / nc, ty * height / nr);
    const uint2 end = uint2((tx + 1u) * width / nc, (ty + 1u) * height / nr);
    const uint2 pixel = start + tid.xy;
    if (any(pixel >= end)) return;
    const float2 uv = (float2(pixel) + 0.5) / size;

    const float4 base = input.SampleLevel(uv, 0);

    const float2 luv = float2(frac(uv.x * float(nc)),
                              frac(uv.y * float(nr)));
    const float4 feed = history.SampleLevel(luv, 0);
    float3 col = feed.rgb;
    float col_a = feed.a;

    const float l = fx_luma(col);
    const float gn =
        0.8 + 0.4 * fx_hash01(fx_hash3(seed ^ 0x2Au, tile, 0x13u));
    col = lerp(col, l * float3(0.78, 0.90, 0.84), saturate(mono)) * gn;
    const float fz =
        fx_hash01(fx_hash4(seed ^ 0xC5u, tid.x, tid.y, frame)) - 0.5;
    col = max(col + fz * saturate(mono) * 0.05 * col_a, 0.0);

    const float o = saturate(osd);
    if (o > 0.01) {
        const float2 tpx = luv * float2(canvas_w, canvas_h) / float2(float(nc), float(nr));
        const float2 tsz = float2(canvas_w, canvas_h) / float2(float(nc), float(nr));
        const float bd =
            min(min(tpx.x, tsz.x - tpx.x), min(tpx.y, tsz.y - tpx.y));
        if (bd < 2.0) { col = lerp(col, float3(0.02, 0.02, 0.02), o);
                        col_a += o * (1.0 - col_a); }
        else if (bd < 3.0) { col = lerp(col, float3(0.35, 0.35, 0.35), o);
                             col_a += o * (1.0 - col_a); }

        const float t = fps > 0.0 ? float(frame) / fps : 0.0;
        const bool blink =
            frac(t * 1.2 + float(tile) * 0.37) < 0.55;
        const float2 dotc = float2(10.0, 10.0);
        if (blink && length(tpx - dotc) < 3.5) {
            col = lerp(col, float3(1.0, 0.12, 0.08), o);
            col_a += o * (1.0 - col_a);
        }

        const float2 dpx = tpx - float2(8.0, tsz.y - 9.0);
        if (dpx.y >= 0.0 && dpx.y < 3.0 && dpx.x >= 0.0) {
            const float slot_f = dpx.x / 6.0;
            const uint slot_i = uint(max(slot_f, 0.0));
            if (slot_i <= tile && frac(slot_f) < 0.5) {
                col = lerp(col, float3(0.95, 0.95, 0.9), o);
                col_a += o * (1.0 - col_a);
            }
        }
    }

    const float4 result = float4(col, col_a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[pixel] = lerp(base, wetted, saturate(opacity));
}
