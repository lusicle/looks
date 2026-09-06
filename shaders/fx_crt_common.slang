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
    float curvature;
    float scanline;
    float lines;
    float bloom;
    float mask_mode;
    float mask_amt;
    float triad;
    float converge;
    float halation;
    float beam_width;
    float bandwidth;
    float persistence;
    float interlaced;
    float canvas_width;
    float canvas_height;
    uint history_valid;
};

float3 crt_erf(float3 x)
{
    float3 t = 1.0 / (1.0 + 0.3275911 * abs(x));
    float3 p = (((1.061405429 * t - 1.453152027) * t +
                  1.421413741) * t - 0.284496736) * t + 0.254829592;
    return sign(x) * (1.0 - p * t * exp(-x * x));
}

float3 crt_beam(float distance, float3 sigma, float footprint)
{
    float half_pixel = max(footprint, 0.001) * 0.5;
    float3 scale = 0.707106781 / sigma;
    return max(0.0, (crt_erf((distance + half_pixel) * scale) -
                     crt_erf((distance - half_pixel) * scale)) /
                    (4.0 * half_pixel));
}
