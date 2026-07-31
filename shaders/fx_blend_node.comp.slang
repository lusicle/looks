// Blend node (docs/flow_canvas.md v3): the graph merge — blends the B
// aux input over In with a classic operator. fx(In) = op(In, B); the
// shared composition (wet interior, opacity outside) gives the mix.
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
    float mode;   // 0 normal, 1 add, 2 multiply, 3 screen, 4 difference,
                  // 5 subtract, 6 darken, 7 lighten, 8 overlay
};

[[vk::binding(0, 0)]] Sampler2D input;
[[vk::binding(1, 0)]] Sampler2D input_b;
[[vk::binding(2, 0)]] RWTexture2D<float4> output;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    const float4 base = input.SampleLevel(uv, 0);
    const float3 a = base.rgb;
    const float3 b = input_b.SampleLevel(uv, 0).rgb;

    const int op = int(mode + 0.5);
    float3 fx = b;                                    // normal
    if (op == 1) fx = a + b;                          // add
    else if (op == 2) fx = a * b;                     // multiply
    else if (op == 3) fx = 1.0 - (1.0 - a) * (1.0 - b);   // screen
    else if (op == 4) fx = abs(a - b);                // difference
    else if (op == 5) fx = max(a - b, 0.0);           // subtract
    else if (op == 6) fx = min(a, b);                 // darken
    else if (op == 7) fx = max(a, b);                 // lighten
    else if (op == 8) {                               // overlay
        const float3 lo = 2.0 * a * b;
        const float3 hi = 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
        fx = lerp(lo, hi, step(0.5, a));
    }

    const float3 wetted = lerp(a, fx, saturate(wet));
    output[tid.xy] = float4(lerp(a, wetted, saturate(opacity)), base.a);
}
