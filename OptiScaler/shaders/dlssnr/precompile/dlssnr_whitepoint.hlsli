// The white point the encode divides a linear HDR frame by, recomputed from a live 1x1 exposure texture when one is
// bound (shared by dlssnr.hlsl and dlssnr_lut.hlsl, so the LUT grades for exactly the white the model is shown).
//
// The including shader declares, before the #include: the cbuffer fields gWhitePoint, gUseGameExposure,
// gUseExposureWhitePoint, gPreExposure, gExposureBaseScale, gExposureTrim, gExposureTrimPreview,
// gExposureTrimAnchorCount and gExposureTrimAnchorExposure0..7 / gExposureTrimAnchorTrim0..7 (DlssNrConstants'
// layout; DlssNrTrim::FillConstants writes the table), and float ExposureSample(), the live texel.

// The Trim that applies at a given base white point (PreExposure / exposure): the slider, unless
// anchors exist and preview is off, in which case the anchors' Trim is interpolated in log space
// between the two that bracket the key, and held flat beyond the first and last.
float ExposureTrimAnchorKey(uint index)
{
    if (index == 0) return gExposureTrimAnchorExposure0;
    if (index == 1) return gExposureTrimAnchorExposure1;
    if (index == 2) return gExposureTrimAnchorExposure2;
    if (index == 3) return gExposureTrimAnchorExposure3;
    if (index == 4) return gExposureTrimAnchorExposure4;
    if (index == 5) return gExposureTrimAnchorExposure5;
    if (index == 6) return gExposureTrimAnchorExposure6;
    return gExposureTrimAnchorExposure7;
}

float ExposureTrimAnchorValue(uint index)
{
    if (index == 0) return gExposureTrimAnchorTrim0;
    if (index == 1) return gExposureTrimAnchorTrim1;
    if (index == 2) return gExposureTrimAnchorTrim2;
    if (index == 3) return gExposureTrimAnchorTrim3;
    if (index == 4) return gExposureTrimAnchorTrim4;
    if (index == 5) return gExposureTrimAnchorTrim5;
    if (index == 6) return gExposureTrimAnchorTrim6;
    return gExposureTrimAnchorTrim7;
}

float EffectiveExposureTrim(float key)
{
    const float fallback = clamp(gExposureTrim, 0.25, 50.0);
    const uint count = min(gExposureTrimAnchorCount, 8u);
    if (gExposureTrimPreview != 0 || count == 0 || !isfinite(key) || key <= 1e-8)
        return fallback;
    if (count == 1)
        return clamp(ExposureTrimAnchorValue(0), 0.25, 50.0);
    const float firstKey = ExposureTrimAnchorKey(0);
    const float lastKey = ExposureTrimAnchorKey(count - 1);
    if (key <= firstKey)
        return clamp(ExposureTrimAnchorValue(0), 0.25, 50.0);
    if (key >= lastKey)
        return clamp(ExposureTrimAnchorValue(count - 1), 0.25, 50.0);

    // The loop is uniform: the exposure and the anchor table are the same for the whole dispatch.
    [loop] for (uint i = 0; i < 7u; ++i)
    {
        if (i + 1 >= count)
            break;
        const float aKey = ExposureTrimAnchorKey(i);
        const float bKey = ExposureTrimAnchorKey(i + 1);
        if (key >= aKey && key <= bKey && bKey > aKey * 1.000001)
        {
            const float aTrim = max(ExposureTrimAnchorValue(i), 0.25);
            const float bTrim = max(ExposureTrimAnchorValue(i + 1), 0.25);
            const float t = (log(key) - log(aKey)) / (log(bKey) - log(aKey));
            return clamp(exp(lerp(log(aTrim), log(bTrim), t)), 0.25, 50.0);
        }
    }
    return clamp(ExposureTrimAnchorValue(count - 1), 0.25, 50.0);
}

// The live sample's base white point multiplier: the learned calibration while Automatic follows the game, else 1.
float ExposureBaseScale()
{
    return (isfinite(gExposureBaseScale) && gExposureBaseScale > 0.0) ? gExposureBaseScale : 1.0;
}

// The picture white point. Sources 0 (paper white) and 2 (scan) resolve it on the CPU and pass it in
// gWhitePoint. Source 1 (the game's own exposure) and source 3 (automatic exposure) also pass a CPU
// value in gWhitePoint as a fallback, but when their 1x1 exposure texture is bound the white point is
// recomputed HERE from the live exposure -- PreExposure / exposure, times the Trim that applies at that
// base white point -- which removes the 3-4 frame CPU-readback lag the meter path has. The clamp
// matches the CPU path's [0.01, 4096]. Vulkan sets only gUseExposureWhitePoint, for Automatic's image and, once the
// host has read the game's own image cleanly through its layout guess, for that (DlssNr_ExposureState.h LiveGameGate).
// Neither backend binds the game's texture under Game exposure's Native look, whose base is 1 whatever the exposure.
float WhitePoint()
{
    if (gUseGameExposure != 0 || gUseExposureWhitePoint != 0)
    {
        const float e = ExposureSample();
        if (isfinite(e) && e > 1e-8 && e < 1e8)
        {
            const float preExposure =
                (isfinite(gPreExposure) && gPreExposure > 1e-6) ? gPreExposure : 1.0;
            const float baseWhitePoint = preExposure / e * ExposureBaseScale();
            return clamp(baseWhitePoint * EffectiveExposureTrim(baseWhitePoint), 0.01, 4096.0);
        }
        // A missing or absurd sample falls through to the CPU value the meter path still maintains.
    }
    return max(gWhitePoint, 1e-4);
}
