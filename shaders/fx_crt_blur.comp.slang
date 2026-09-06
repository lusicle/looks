[[vk::push_constant]]
cbuffer Push
{
    uint width;
    uint height;
    uint pass;
    float sigma;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    float2 size = float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) / size;
    float4 color = 0.0;
    if (pass == 0) {
        float2 d = 0.25 / size;
        color = (input.SampleLevel(uv + float2(-d.x, -d.y), 0) +
                 input.SampleLevel(uv + float2(d.x, -d.y), 0) +
                 input.SampleLevel(uv + float2(-d.x, d.y), 0) +
                 input.SampleLevel(uv + float2(d.x, d.y), 0)) * 0.25;
    } else {
        float total = 0.0;
        float2 direction = pass == 1 ? float2(1.0 / size.x, 0.0) :
                                       float2(0.0, 1.0 / size.y);
        int radius = int(ceil(3.0 * sigma));
        for (int i = -radius; i <= radius; ++i) {
            float weight = exp(-0.5 * float(i * i) / (sigma * sigma));
            color += input.SampleLevel(uv + direction * float(i), 0) * weight;
            total += weight;
        }
        color /= total;
    }
    output[tid.xy] = color;
}
