// The proxy curves whose answer can replace the frame (Neutwo, the hybrid), the sRGB pair they are stored through, and
// the ratio floor. Shared by dlssnr.hlsl and dlssnr_detail_reuse.hlsl, which lands a reused Replace change in the light
// these decode to. Pure functions and one constant: no resources.
#ifndef DLSSNR_REPLACE_CURVE_HLSLI
#define DLSSNR_REPLACE_CURVE_HLSLI

// The dual-floor ratio idiom's shared constant (see its own fuller explanation where it is first
// used, around `lumaRatio` in dlssnr.hlsl's resolve): added to both sides of a luminance ratio so it falls
// smoothly to 1 near black instead of diverging. At file scope, not a local inside
// CSMain, so `ApplyReplaceGuard` and the reuse shader share the exact same floor rather than either
// duplicating the literal or threading it through as a parameter.
static const float kRatioFloor = 1.0 / 512.0;

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// The reversible proxy, from RenoDX's Sep-2 DLSS 5 addon (clshortfuse) -- an unclipped, hue-preserving
// encode meant to be reproduced exactly, so the model is shown the highlight gradation the soft knee
// compresses into a razor-thin band near white. Neutwo maps [0, inf) -> [0, 1) with no clip point,
// applied as ONE scalar on the peak channel so the three channels keep their ratios and hue cannot
// bend. The knee reaches its asymptote within a stop of white -- scene 2 and scene 4 arrive ~0.001
// apart, nothing the model can resolve between; Neutwo puts them ~0.076 apart.
//
// This changes only WHAT the proxy is. The resolve already works in a hybrid space -- proxy and model
// in display [0,1], original in linear -- and its ratio bridges the two, so nothing downstream has to
// change: the proxy is decoded by the same SrgbToLinear, compared the same way, and the ratio carries
// the model's answer back to the original's linear luminance exactly as before. Only the matched-
// residual proxy rebuild, which reproduces the encode, switches curve with it.
//
// Gamut nuance (RenoDX also compresses toward the D65 neutral axis first) is deferred: out-of-BT.709
// negative channels are clamped to zero here, enough for the highlight question this measures. See
// Licenses/RenoDX_LICENSE.txt.
float Neutwo(float x) { return x * rsqrt(x * x + 1.0); } // [0, inf) -> [0, 1), no clip point

float3 NeutwoEncode(float3 v)
{
    v = max(v, 0.0);
    float m = max(v.r, max(v.g, v.b));

    if (m <= 1e-6)
        return v;

    // One scalar taken from the peak channel keeps the hue; the peak lands at Neutwo(m) < 1, so no
    // channel clips and LinearToSrgb's saturate never fires -- the proxy is fully invertible.
    return v * (Neutwo(m) / m);
}

// The exact inverse of NeutwoEncode, for the "replace" decode: y/sqrt(1 - y^2) on the peak channel,
// same one-scalar-preserves-hue trick. The inverse diverges at 1, so the peak is clamped just below
// it -- this is the steep-highlight-slope the toggle's help warns about: a highlight at the ceiling
// decodes to a very large but finite value. Only the replace path uses this; the composed path never
// decodes (its ratio bridges display->linear instead), so mode 0/1 are untouched by it.
float3 NeutwoDecode(float3 y)
{
    y = max(y, 0.0);
    float m = max(y.r, max(y.g, y.b));
    m = min(m, 0.999999);

    if (m <= 1e-6)
        return y;

    float x = m * rsqrt(max(1.0 - m * m, 1e-8)); // Neutwo^-1 of the peak
    return y * (x / m);
}

// The hybrid proxy (mode 3): the fix for the two curves each only winning in some scenes. The soft
// knee is fine in the midtones but crushes highlights; Neutwo fixes the highlights but compresses the
// midtones too, so it only helps where the knee was hurting (bright content) and is a downgrade in
// soft-lit content. The hybrid is IDENTITY below the knee -- so midtones are exactly what the soft
// knee already gave (as good as Off) -- and an unclipped, gentle Neutwo-of-the-excess ABOVE it, so
// highlights get the gradation the model needs. C1-continuous at the knee. One proxy that is >= Off
// everywhere: no midtone loss, plus the highlight win. (Composed only -- mode 3 does not replace.)
float HybridCurve(float m)
{
    const float k = 0.75; // knee point: identity below, gentle unclipped roll above

    if (m <= k)
        return m;

    const float e = (m - k) / (1.0 - k);         // excess above the knee, [0, inf)
    return k + (1.0 - k) * (e * rsqrt(e * e + 1.0)); // Neutwo(e) scaled into [k, 1); -> 1, never clips
}

float3 HybridEncode(float3 v)
{
    v = max(v, 0.0);
    float m = max(v.r, max(v.g, v.b));

    if (m <= 1e-6)
        return v;

    // One scalar on the peak channel, hue preserved. Below the knee the scalar is 1 (identity); above
    // it the peak lands at HybridCurve(m) < 1, so no channel clips.
    return v * (HybridCurve(m) / m);
}

// The exact inverse of the hybrid curve, for the hybrid REPLACE decode (mode 4). Because it is IDENTITY
// below the knee, the steep expansion is confined to genuine highlights: midtone model wobble is not
// amplified, so hybrid-replace flashes far less than Neutwo-replace while keeping the raw model detail.
float HybridCurveInv(float y)
{
    const float k = 0.75;

    if (y <= k)
        return y;

    float u = (y - k) / (1.0 - k);                  // Neutwo(e), in [0,1)
    u = min(u, 0.999999);                           // the inverse diverges at 1
    const float e = u * rsqrt(max(1.0 - u * u, 1e-8)); // Neutwo^-1 of the excess
    return k + (1.0 - k) * e;
}

float3 HybridDecode(float3 y)
{
    y = max(y, 0.0);
    float m = max(y.r, max(y.g, y.b));

    if (m <= 1e-6)
        return y;

    return y * (HybridCurveInv(m) / m);
}

#endif // DLSSNR_REPLACE_CURVE_HLSLI
