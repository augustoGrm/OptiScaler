// HDR10 (SMPTE ST 2084 PQ, BT.2020 primaries) <-> linear BT.709 light in scRGB units (1 = 80 nits).
// Shared by dlssnr_finished_color.hlsl (Finished Picture's own conversion pass) and dlssnr.hlsl (the Colour encoding
// override's PQ edge), so both read a PQ frame the same way. Negative components carry colours outside BT.709.
#ifndef DLSSNR_PQ_HLSLI
#define DLSSNR_PQ_HLSLI

float3 DecodePQ(float3 code)
{
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(code), 1.0 / m2);
    return pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-6), 1.0 / m1) * 125.0;
}
float3 EncodePQ(float3 light)
{
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(light / 125.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
static const float3x3 kBt2020To709 = {
     1.6604910, -0.5876411, -0.0728499,
    -0.1245505,  1.1328999, -0.0083494,
    -0.0181508, -0.1005789,  1.1187297 };
static const float3x3 kBt709To2020 = {
    0.6274039, 0.3292830, 0.0433131,
    0.0690973, 0.9195404, 0.0113623,
    0.0163914, 0.0880133, 0.8955953 };

#endif
