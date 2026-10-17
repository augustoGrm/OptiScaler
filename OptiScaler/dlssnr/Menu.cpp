#include "Settings.h"
#include "Runtime.h"
#include <imgui/imgui.h>

namespace DlssNr
{
void RenderMenu()
{
    auto settings = GetSettings();
    bool changed = ImGui::Checkbox("Enable DLSS Neural Rendering (DX12)", &settings.enabled);
    ImGui::TextWrapped("Requires nvngx_dlssnr.dll for your GPU beside OptiScaler. The first model load can take time.");
    ImGui::TextWrapped("%s", GetStatus().c_str());
    if (ImGui::Button("Retry model initialization"))
        Retry();
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Effect strength", &settings.strength, 0, 2);
    changed |= ImGui::SliderFloat("Colour strength", &settings.colourStrength, 0, 2);
    changed |= ImGui::SliderFloat("Model resolution", &settings.scale, 0.25f, 1.0f, "%.2fx");
    int passes = static_cast<int>(settings.passes);
    if (ImGui::SliderInt("Passes", &passes, 1, 3))
    {
        settings.passes = passes;
        changed = true;
    }
    changed |= ImGui::SliderFloat("Pass feedback", &settings.feedback, 0, 1);
    int preset = static_cast<int>(settings.preset), style = static_cast<int>(settings.style);
    if (ImGui::Combo("Model preset", &preset, "Default\0Preset 1\0Preset 2\0Preset 3\0"))
    {
        settings.preset = preset;
        changed = true;
    }
    if (ImGui::Combo("Model style", &style, "Standard\0Natural\0Cinematic\0"))
    {
        settings.style = style;
        changed = true;
    }
    changed |= ImGui::SliderFloat("Intensity", &settings.intensity, 0, 2);
    changed |= ImGui::SliderFloat("Local structure", &settings.localStructure, 0, 2);
    changed |= ImGui::SliderFloat("Local tone", &settings.localTone, 0, 2);
    changed |= ImGui::SliderFloat("Skin structure (-1 follows local)", &settings.skinStructure, -1, 2);
    changed |= ImGui::Checkbox("Automatic skin mask", &settings.autoMask);
    ImGui::Separator();
    int encoding = static_cast<int>(settings.colourEncoding);
    if (ImGui::Combo("Colour encoding", &encoding, "Auto\0Linear HDR\0Tone-mapped sRGB\0Gamma 2.2\0PQ (HDR10)\0"))
    {
        settings.colourEncoding = encoding;
        changed = true;
    }
    changed |= ImGui::Checkbox("Use game exposure when available", &settings.gameExposure);
    changed |= ImGui::DragFloat("Fallback white point", &settings.whitePoint, 0.01f, 0.01f, 10000.0f);
    changed |= ImGui::SliderFloat("Maximum brightening ratio", &settings.maxRatio, 1, 10);
    ImGui::TextWrapped("Save with OptiScaler's existing Save configuration button. Frame Generation and latency remain "
                       "in their original tabs.");
    if (changed)
        SetSettings(settings);
}
} // namespace DlssNr
