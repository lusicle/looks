// Binding 1 holds the phase field from the separate mod_integrate pass.

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
    float omega;
    float distortion;
    float lowpass;      // the integrator pass uses this, not the shader
    float speed;
    float mode;
    float line_w;
    float channels;
    float spread;
    float color;
    float threshold;    // the integrator pass uses this, not the shader
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D integral;   // RGBA32F phase field
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

// Keep the slope clamp. An unclamped slope fills the traces solid.
float fm_trace(float phi, float f_px, float slope_px, float lw)
{
    const float dc = phi - round(phi);
    const float d_px = abs(dc) / max(f_px, 1e-4);
    const float slant = min(abs(slope_px), 2.5);
    const float dist = max(d_px - slant * 0.75, 0.0);
    float a = saturate(1.0 - dist / lw);
    // A crest inside this pixel must light, or dense regions alias to black.
    if (abs(dc) <= f_px * 0.6) a = max(a, 0.8);
    return a * a;
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

    const bool vert = mode >= 0.5;
    const float t = fps > 0.0 ? float(frame) / fps : 0.0;
    const float lw = max(line_w, 0.5);
    const uint n_fam = uint(clamp(channels, 1.0, 4.0) + 0.5);

    const float2 d_march =
        (vert ? float2(1.0, 0.0) : float2(0.0, 1.0)) / size;
    const float2 d_lane =
        (vert ? float2(0.0, 1.0) : float2(1.0, 0.0)) / size;
    const float4 phi = integral.SampleLevel(uv, 0);
    const float4 phi_prev =
        integral.SampleLevel(clamp(uv - d_march, 0.0, 1.0), 0);
    const float4 phi_l =
        integral.SampleLevel(clamp(uv - d_lane, 0.0, 1.0), 0);
    const float4 phi_r =
        integral.SampleLevel(clamp(uv + d_lane, 0.0, 1.0), 0);

    // Use abs. A reversed march accumulates phase the other way.
    const float4 f_px = max(abs(phi - phi_prev), 1e-5);   // cycles/px
    const float4 dlane = (phi_r - phi_l) * 0.5;           // cycles/px

    // The integrator owns the speed sign. travel uses abs here.
    const float travel = -abs(speed) * t;
    const float3 tint[3] = {
        float3(1.0, 0.12, 0.10),
        float3(0.10, 1.0, 0.14),
        float3(0.12, 0.16, 1.0),
    };

    float3 col = float3(0.0, 0.0, 0.0);
    for (uint g = 0; g < n_fam; ++g) {
        const float scale = 1.0 + spread * 0.3 * float(g);
        const float offs = float(g) / float(n_fam) + travel;
        const float wl = fm_trace(phi.a * scale + offs, f_px.a * scale,
                                  dlane.a * scale / (f_px.a * scale),
                                  lw);
        float3 rgb_l = float3(0.0, 0.0, 0.0);
        if (color > 0.001) {
            [unroll]
            for (int c = 0; c < 3; ++c) {
                const float tr = fm_trace(
                    phi[c] * scale + offs, f_px[c] * scale,
                    dlane[c] * scale / (f_px[c] * scale), lw);
                rgb_l += tint[c] * tr;
            }
        }
        col = max(col, lerp(float3(wl, wl, wl), rgb_l,
                            saturate(color)));
    }

    const float4 result = float4(min(col, float3(1.4, 1.4, 1.4)),
                                 base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
