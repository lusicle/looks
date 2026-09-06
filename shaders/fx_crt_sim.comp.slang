#include "fx_crt_common.slang"

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D raster;
[[vk::binding(2, 0)]] Sampler2D scatter;
[[vk::binding(3, 0)]] RWTexture2D<float4> output;

float crt_integral(float x, float duty)
{
    return floor(x) * duty + min(frac(x), duty);
}

float crt_gate(float position, float footprint, float period, float duty)
{
    float p = position / period + duty * 0.5;
    float w = max(footprint / period, 0.0001);
    return (crt_integral(p + w * 0.5, duty) -
            crt_integral(p - w * 0.5, duty)) / w;
}

float3 crt_mask(float2 p, float2 footprint, int mode)
{
    float pitch = max(triad, 2.0);
    float3 color = 0.0;
    if (mode == 2) {
        float2 f = footprint / pitch;
        float2 variance = 0.0484 + f * f / 12.0;
        float gain = 0.0484 / sqrt(variance.x * variance.y);
        for (int channel = 0; channel < 3; ++channel) {
            float value = 0.0;
            for (int row = 0; row < 2; ++row) {
                float2 q = p / pitch - float2(float(channel) + 0.5 -
                                             float(row) * 0.5, float(row) + 0.5);
                float2 d = q - floor(q / float2(3, 2) + 0.5) * float2(3, 2);
                value += exp(-0.5 * dot(d * d, 1.0 / variance)) * gain;
            }
            color[channel] = value / (6.283185307 * 0.0484 / 3.0);
        }
        return lerp(color, float3(1.0), smoothstep(0.65, 1.5, max(f.x, f.y)));
    }
    for (int channel = 0; channel < 3; ++channel) {
        if (mode == 1) {
            for (int column = 0; column < 2; ++column) {
                int element = channel + column * 3;
                float x = crt_gate(p.x - (float(element) + 0.5) * pitch,
                                   footprint.x, 6.0 * pitch, 0.82 / 6.0);
                float y = crt_gate(p.y - float(element & 1) * pitch * 1.25,
                                   footprint.y, pitch * 2.5, 0.85);
                y = lerp(y, 0.85, smoothstep(0.35, 0.5, footprint.y / (pitch * 2.5)));
                color[channel] += x * y;
            }
            color[channel] *= 3.0 / (0.82 * 0.85);
        } else {
            color[channel] = crt_gate(p.x - (float(channel) + 0.5) * pitch,
                                     footprint.x, 3.0 * pitch, 0.82 / 3.0) *
                             (3.0 / 0.82);
        }
    }
    return lerp(color, float3(1.0), smoothstep(0.35, 0.5, footprint.x / (3.0 * pitch)));
}

float4 crt_lcd(float2 uv, float2 footprint)
{
    float2 p = uv * float2(canvas_width, canvas_height);
    float pitch = max(triad, 2.0);
    float3 mask = crt_mask(p, footprint, 0);
    float rows = crt_gate(p.y, footprint.y, pitch, 0.85) / 0.85;
    return input.SampleLevel(uv, 0) *
           float4(lerp(float3(1.0), mask * lerp(1.0, rows, saturate(scanline)),
                       saturate(mask_amt)), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 size = float2(width, height);
    float2 canvas = float2(canvas_width, canvas_height);
    float2 uv = (float2(tid.xy) + 0.5) / size;
    float4 base = input.SampleLevel(uv, 0);
    if (mask_mode >= 2.5) {
        output[tid.xy] = lerp(base, crt_lcd(uv, canvas / size),
                             saturate(wet) * saturate(opacity));
        return;
    }

    float2 c = uv * 2.0 - 1.0;
    float2 aspect = canvas / max(canvas.x, canvas.y);
    float2 metric = aspect * aspect;
    float k = curvature * 0.22;
    float radius = dot(c * c, metric);
    float bend = 1.0 + k * radius;
    float2 surface = (c * bend + 1.0) * 0.5;
    float2 dx = float2(bend + 2.0 * k * c.x * c.x * metric.x,
                      2.0 * k * c.x * c.y * metric.x) / size.x;
    float2 dy = float2(2.0 * k * c.x * c.y * metric.y,
                      bend + 2.0 * k * c.y * c.y * metric.y) / size.y;
    float2 footprint = abs(dx) + abs(dy);
    float2 edge = saturate((0.5 - abs(surface - 0.5)) / footprint + 0.5);
    float coverage = edge.x * edge.y;
    float4 result = 0.0;
    if (coverage > 0.0) {
        float raster_lines = max(64.0, floor(lines + 0.5));
        float pixel_lines = footprint.y * raster_lines;
        float4 direct;
        if (pixel_lines > 1.0) {
            float2 d = footprint * 0.25;
            direct = (raster.SampleLevel(surface + float2(-d.x, -d.y), 0) +
                      raster.SampleLevel(surface + float2(d.x, -d.y), 0) +
                      raster.SampleLevel(surface + float2(-d.x, d.y), 0) +
                      raster.SampleLevel(surface + float2(d.x, d.y), 0)) * 0.25;
        } else {
            direct = raster.SampleLevel(surface, 0);
        }
        float4 beam = direct;
        float scan = saturate(scanline) * (1.0 - smoothstep(0.4, 0.5, pixel_lines));
        if (scan > 0.0) {
            float y = surface.y * raster_lines;
            float first = floor(y - 0.5) - 2.0 + 0.5;
            float3 light = 0.0;
            float alpha = 0.0;
            float alpha_weight = 0.0;
            for (int i = 0; i < 6; ++i) {
                float center = first + float(i);
                if (center < 0.0 || center >= raster_lines) continue;
                float4 sample = raster.SampleLevel(float2(surface.x, center / raster_lines), 0);
                float3 level = sample.a > 0.0001 ? sample.rgb / sample.a : float3(0.0);
                float3 sigma = clamp(beam_width + bloom * 0.35 *
                                     pow(saturate(level), float3(0.6)), 0.15, 0.8);
                float3 weight = crt_beam(y - center, sigma, pixel_lines);
                light += sample.rgb * weight;
                float aw = max(weight.r, max(weight.g, weight.b));
                alpha += sample.a * aw;
                alpha_weight += aw;
            }
            beam = float4(light, alpha / max(alpha_weight, 0.0001));
        }
        result = lerp(direct, beam, scan);
        if (mask_amt > 0.0) {
            float3 mask = crt_mask(surface * canvas, footprint * canvas, int(mask_mode + 0.5));
            result.rgb *= lerp(float3(1.0), mask, saturate(mask_amt));
        }
        if (halation > 0.0) {
            float4 glow = scatter.SampleLevel(surface, 0);
            result = lerp(result, glow, saturate(halation) * 0.25);
        }
        result.rgb *= saturate(1.0 - radius * curvature * 0.12);
        result *= coverage;
    }
    output[tid.xy] = lerp(base, result, saturate(wet) * saturate(opacity));
}
