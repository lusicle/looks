// One Gray-Scott reaction-diffusion step. State is (A, B) in
// RG16F; the frame seeds the sim by injecting B where the input has
// midtone luma — the picture literally grows the pattern. No randomness:
// the sim is deterministic given the sequential frame stream.

[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    float feed;
    float kill;
    float inject;   // frame-luma B injection strength
};

[[vk::binding(0, 0)]] Sampler2D state;     // A = r, B = g
[[vk::binding(1, 0)]] Sampler2D frame_in;  // linear RGB source
[[vk::binding(2, 0)]] RWTexture2D<float4> next_state;

float2 ab_at(float2 uv)
{
    return state.SampleLevel(uv, 0).rg;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float2 e = 1.0 / size;

    const float2 c = ab_at(uv);
    // 3x3 Laplacian: adjacent 0.2, diagonal 0.05, center -1.
    float2 lap = -c;
    lap += 0.2 * ab_at(uv + float2(e.x, 0.0));
    lap += 0.2 * ab_at(uv - float2(e.x, 0.0));
    lap += 0.2 * ab_at(uv + float2(0.0, e.y));
    lap += 0.2 * ab_at(uv - float2(0.0, e.y));
    lap += 0.05 * ab_at(uv + e);
    lap += 0.05 * ab_at(uv - e);
    lap += 0.05 * ab_at(uv + float2(e.x, -e.y));
    lap += 0.05 * ab_at(uv - float2(e.x, -e.y));

    const float abb = c.x * c.y * c.y;
    float a = c.x + (1.0 * lap.x - abb + feed * (1.0 - c.x));
    float b = c.y + (0.5 * lap.y + abb - (feed + kill) * c.y);

    // Seed from the frame: midtone luma feeds B where the sim is quiet.
    const float3 rgb = frame_in.SampleLevel(uv, 0).rgb;
    const float l = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    const float mid = smoothstep(0.05, 0.25, l) * (1.0 - smoothstep(0.5, 0.9, l));
    b += inject * 0.02 * mid * (1.0 - b);

    next_state[tid.xy] = float4(saturate(a), saturate(b), 0.0, 1.0);
}
