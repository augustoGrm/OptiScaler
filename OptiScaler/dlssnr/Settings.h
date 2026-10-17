#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr
{
struct Settings
{
    bool enabled = false;
    uint32_t preset = 0;
    uint32_t style = 0;
    uint32_t passes = 1;
    uint32_t colourEncoding = 0;
    float intensity = 1.0f;
    float localStructure = 1.0f;
    float localTone = 1.0f;
    float skinStructure = -1.0f;
    bool autoMask = true;
    float strength = 1.0f;
    float colourStrength = 1.0f;
    float whitePoint = 1.0f;
    bool gameExposure = true;
    float maxRatio = 2.0f;
    float scale = 1.0f;
    float feedback = 1.0f;

    bool operator==(const Settings&) const = default;
};

inline Settings Sanitize(Settings s)
{
    auto bound = [](float v, float fallback, float lo, float hi)
    { return std::clamp(std::isfinite(v) ? v : fallback, lo, hi); };
    s.preset = std::min(s.preset, 3u);
    s.style = std::min(s.style, 2u);
    s.passes = std::clamp(s.passes, 1u, 3u);
    s.colourEncoding = std::min(s.colourEncoding, 4u);
    s.intensity = bound(s.intensity, 1, 0, 2);
    s.localStructure = bound(s.localStructure, 1, 0, 2);
    s.localTone = bound(s.localTone, 1, 0, 2);
    s.skinStructure = bound(s.skinStructure, -1, -1, 2);
    s.strength = bound(s.strength, 1, 0, 2);
    s.colourStrength = bound(s.colourStrength, 1, 0, 2);
    s.whitePoint = bound(s.whitePoint, 1, 0.01f, 10000);
    s.maxRatio = bound(s.maxRatio, 2, 1, 10);
    s.scale = bound(s.scale, 1, 0.25f, 1);
    s.feedback = bound(s.feedback, 1, 0, 1);
    return s;
}

inline bool ShouldRun(const Settings& s) { return s.enabled && s.strength > 0.0f; }
Settings GetSettings();
void SetSettings(Settings settings);
void RenderMenu();
} // namespace DlssNr
