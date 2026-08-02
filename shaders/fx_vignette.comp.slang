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
    float amount;
    float radius;      // where falloff starts
    float softness;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);

    const float2 centered = uv * 2.0 - 1.0;
    const float dist = length(centered) * 0.7071;
    const float soft = max(softness, 1.0e-3);
    const float falloff = smoothstep(radius, radius + soft, dist);
    const float darken = 1.0 - saturate(amount) * falloff;

    const float4 result = float4(base.rgb * darken, base.a);
    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
