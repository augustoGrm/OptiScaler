#pragma once
#include "Settings.h"

namespace DlssNr
{
// Uses the host's existing SimpleIni document, so SaveIni retains every other section.
template <class Ini> Settings ReadSettings(const Ini& ini)
{
    Settings s;
    constexpr auto section = "DlssNr";
#define NR_BOOL(field, key) s.field = ini.GetBoolValue(section, key, s.field)
#define NR_INT(field, key) s.field = static_cast<uint32_t>(std::max(0L, ini.GetLongValue(section, key, s.field)))
#define NR_FLOAT(field, key) s.field = static_cast<float>(ini.GetDoubleValue(section, key, s.field))
    NR_BOOL(enabled, "Enabled");
    NR_INT(preset, "Preset");
    NR_INT(style, "Style");
    NR_INT(passes, "PassCount");
    NR_INT(colourEncoding, "ColourEncoding");
    NR_FLOAT(intensity, "Intensity");
    NR_FLOAT(localStructure, "LocalStructure");
    NR_FLOAT(localTone, "LocalTone");
    NR_FLOAT(skinStructure, "SkinStructure");
    NR_BOOL(autoMask, "AutoMask");
    NR_FLOAT(strength, "TransferStrength");
    NR_FLOAT(colourStrength, "ColourStrength");
    NR_FLOAT(whitePoint, "WhitePoint");
    NR_BOOL(gameExposure, "GameExposure");
    NR_FLOAT(maxRatio, "MaxRatio");
    NR_FLOAT(scale, "WorkScale");
    NR_FLOAT(feedback, "PassFeedback");
#undef NR_BOOL
#undef NR_INT
#undef NR_FLOAT
    return Sanitize(s);
}

template <class Ini> void WriteSettings(Ini& ini, const Settings& value)
{
    const auto s = Sanitize(value);
    constexpr auto section = "DlssNr";
#define NR_BOOL(field, key) ini.SetBoolValue(section, key, s.field)
#define NR_INT(field, key) ini.SetLongValue(section, key, s.field)
#define NR_FLOAT(field, key) ini.SetDoubleValue(section, key, s.field)
    NR_BOOL(enabled, "Enabled");
    NR_INT(preset, "Preset");
    NR_INT(style, "Style");
    NR_INT(passes, "PassCount");
    NR_INT(colourEncoding, "ColourEncoding");
    NR_FLOAT(intensity, "Intensity");
    NR_FLOAT(localStructure, "LocalStructure");
    NR_FLOAT(localTone, "LocalTone");
    NR_FLOAT(skinStructure, "SkinStructure");
    NR_BOOL(autoMask, "AutoMask");
    NR_FLOAT(strength, "TransferStrength");
    NR_FLOAT(colourStrength, "ColourStrength");
    NR_FLOAT(whitePoint, "WhitePoint");
    NR_BOOL(gameExposure, "GameExposure");
    NR_FLOAT(maxRatio, "MaxRatio");
    NR_FLOAT(scale, "WorkScale");
    NR_FLOAT(feedback, "PassFeedback");
#undef NR_BOOL
#undef NR_INT
#undef NR_FLOAT
}
} // namespace DlssNr
