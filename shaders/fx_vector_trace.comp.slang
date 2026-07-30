// Vector Trace: beam strokes crawl along image contours over decaying
// phosphor — the image perpetually being hand-drawn in outline. Seeded
// strokes spawn on edges (one candidate per cell); each is a two-segment
// polyline following the isophote tangent, with a bright head sliding
// along it on the crawl clock. Canvas = own previous output.
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
    float density;   // stroke spawn probability
    float len_px;    // stroke length
    float rate;      // head crawl rate, hz
    float persist;   // phosphor decay
    float beam_w;    // stroke width, px
    float edge;      // how strongly strokes require edges
    float color;     // 0 mono phosphor -> 1 source color
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D canvas;   // own previous output
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

// Luma gradient by central differences (encoded space for stable edges).
float2 vt_grad(Sampler2D tex, float2 uv, float2 size)
{
    const float2 e = 1.5 / size;
    const float lr = fx_luma(tex.SampleLevel(
        clamp(uv + float2(e.x, 0.0), 0.0, 1.0), 0).rgb);
    const float ll = fx_luma(tex.SampleLevel(
        clamp(uv - float2(e.x, 0.0), 0.0, 1.0), 0).rgb);
    const float ld = fx_luma(tex.SampleLevel(
        clamp(uv + float2(0.0, e.y), 0.0, 1.0), 0).rgb);
    const float lu = fx_luma(tex.SampleLevel(
        clamp(uv - float2(0.0, e.y), 0.0, 1.0), 0).rgb);
    return float2(lr - ll, ld - lu);
}

// Distance from p to segment ab, and the normalized parameter along it.
float2 vt_seg(float2 p, float2 a, float2 b)
{
    const float2 ab = b - a;
    const float h = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
    return float2(length(p - (a + ab * h)), h);
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

    float3 col = canvas.SampleLevel(uv, 0).rgb * saturate(persist);

    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const uint epoch = uint(t * 0.25);   // strokes refresh slowly
    const float len = max(len_px, 4.0);
    const float cell = len * 1.25;
    const float bw = max(beam_w, 0.5);
    const float2 p = float2(tid.xy);

    const int2 my_cell = int2(p / cell);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        const int2 c = my_cell + int2(dx, dy);
        if (c.x < 0 || c.y < 0) continue;
        const uint h = fx_hash4(seed ^ (epoch + 1u) * 0x2545u,
                                uint(c.x), uint(c.y), 0x11u);

        const float2 start =
            (float2(c) + float2(fx_hash01(fx_hash(h ^ 0x21u)),
                                fx_hash01(fx_hash(h ^ 0x32u)))) * cell;
        if (start.x >= size.x || start.y >= size.y) continue;
        const float2 suv = (start + 0.5) / size;

        // Spawn gate: edge affinity wants gradient under the start.
        const float2 g = vt_grad(input, suv, size);
        const float gmag = length(g);
        const float prob = saturate(density) *
                           lerp(1.0, saturate(gmag * 8.0), saturate(edge));
        if (fx_hash01(fx_hash(h ^ 0x43u)) >= prob) continue;

        // Two-segment polyline along the contour tangent.
        float2 tan1 = gmag > 1e-5 ? float2(-g.y, g.x) / gmag
                                  : float2(1.0, 0.0);
        if ((h & 0x100u) != 0u) tan1 = -tan1;   // seeded direction
        const float2 mid = start + tan1 * (len * 0.5);
        const float2 muv = clamp((mid + 0.5) / size, 0.0, 1.0);
        const float2 g2 = vt_grad(input, muv, size);
        float2 tan2 = length(g2) > 1e-5 ? normalize(float2(-g2.y, g2.x))
                                        : tan1;
        if (dot(tan2, tan1) < 0.0) tan2 = -tan2;
        const float2 fin = mid + tan2 * (len * 0.5);

        const float2 d1 = vt_seg(p, start, mid);
        const float2 d2 = vt_seg(p, mid, fin);
        const float dist = min(d1.x, d2.x);
        if (dist >= bw + 1.0) continue;
        const float along = d1.x <= d2.x ? d1.y * 0.5 : 0.5 + d2.y * 0.5;

        const float a = saturate(1.0 - dist / bw);
        // Crawling head: bright spot sliding start -> end on the clock.
        const float headf =
            frac(t * rate + fx_hash01(fx_hash(h ^ 0x77u)));
        const float head = exp(-pow((along - headf) * 6.0, 2.0));

        const float3 src =
            input.SampleLevel(clamp((p + 0.5) / size, 0.0, 1.0), 0).rgb;
        const float3 lc =
            lerp(float3(0.35, 1.0, 0.55), src * 1.6, saturate(color));
        col = max(col, lc * a * a * (0.2 + 1.3 * head));
    }

    const float4 result =
        float4(min(col, float3(1.6, 1.6, 1.6)), base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
