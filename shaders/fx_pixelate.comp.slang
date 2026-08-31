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
    float block_size;   // px
    float sample_mode;
    float shape;
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] RWTexture2D<float4> output;

static const int kProbe = 4;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 size = float2(width, height);
    const float2 uv = (float2(tid.xy) + 0.5) / size;
    const float4 base = input.SampleLevel(uv, 0);

    const float block = max(block_size, 1.0);
    float2 cell = float2(block, block);
    if (shape > 0.5 && shape < 1.5)
        cell.y = size.y;
    else if (shape > 1.5)
        cell.x = size.x;

    const float2 origin = floor(float2(tid.xy) / cell) * cell;

    float4 result;
    if (sample_mode < 0.5)
    {
        result = input.SampleLevel((origin + cell * 0.5) / size, 0);
    }
    else if (sample_mode > 4.5)
    {
        result = input.SampleLevel((origin + 0.5) / size, 0);
    }
    else
    {
        float4 acc = 0.0;
        float4 hi = input.SampleLevel((origin + cell * 0.5) / size, 0);
        float4 lo = hi;
        float hi_l = -1.0e9;
        float lo_l = 1.0e9;
        float lums[kProbe * kProbe];
        float4 cols[kProbe * kProbe];
        for (int y = 0; y < kProbe; ++y)
        {
            for (int x = 0; x < kProbe; ++x)
            {
                const float2 f =
                    (float2(x, y) + 0.5) / float(kProbe);
                const float2 suv =
                    clamp((origin + f * cell) / size, 0.0, 1.0);
                const float4 s = input.SampleLevel(suv, 0);
                const float l = fx_luma(s.rgb);
                const int i = y * kProbe + x;
                lums[i] = l;
                cols[i] = s;
                acc += s;
                if (l > hi_l) { hi_l = l; hi = s; }
                if (l < lo_l) { lo_l = l; lo = s; }
            }
        }
        if (sample_mode < 1.5)
        {
            result = acc / float(kProbe * kProbe);
        }
        else if (sample_mode < 2.5)
        {
            result = hi;
        }
        else if (sample_mode < 3.5)
        {
            result = lo;
        }
        else
        {
            const int n = kProbe * kProbe;
            result = cols[0];
            for (int i = 0; i < n; ++i)
            {
                int below = 0;
                for (int j = 0; j < n; ++j)
                    if (lums[j] < lums[i] || (lums[j] == lums[i] && j < i))
                        ++below;
                if (below == n / 2)
                    result = cols[i];
            }
        }
    }

    const float4 wetted = lerp(base, result, saturate(wet));
    output[tid.xy] = lerp(base, wetted, saturate(opacity));
}
