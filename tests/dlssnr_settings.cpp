#include "../OptiScaler/dlssnr/SettingsIni.h"
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include <SimpleIni.h>
#include <cassert>
#include <limits>
#include <string>
#include <iostream>

int main()
{
    using namespace DlssNr;
    CSimpleIniA ini;
    assert(ini.LoadData("[FrameGen]\nExternal=false\nFGNvngxReplacement=Combo\nEnabled=true\n"
                        "[Plugins]\nLoadAsiPlugins=true\n[Shortcuts]\nToggleKey=45\n") == SI_OK);
    auto defaults = ReadSettings(ini);
    assert(!ShouldRun(defaults));
    assert(defaults.passes == 1 && defaults.scale == 1 && defaults.skinStructure == -1);
    Settings selected;
    selected.enabled = true;
    selected.passes = 3;
    selected.scale = 0.5f;
    selected.intensity = 0.75f;
    selected.skinStructure = -1;
    selected.whitePoint = 13.25f;
    selected.colourEncoding = 4;
    selected.feedback = 0.625f;
    WriteSettings(ini, selected);
    std::string saved;
    assert(ini.Save(saved) == SI_OK);
    CSimpleIniA reloaded;
    assert(reloaded.LoadData(saved.c_str()) == SI_OK);
    assert(ReadSettings(reloaded) == selected);
    assert(!reloaded.GetBoolValue("FrameGen", "External", true));
    assert(reloaded.GetBoolValue("FrameGen", "Enabled", false));
    assert(std::string(reloaded.GetValue("FrameGen", "FGNvngxReplacement")) == "Combo");
    assert(reloaded.GetBoolValue("Plugins", "LoadAsiPlugins", false));
    assert(reloaded.GetLongValue("Shortcuts", "ToggleKey") == 45);
    selected.strength = 0;
    assert(!ShouldRun(selected));
    auto malformed = selected;
    malformed.scale = std::numeric_limits<float>::quiet_NaN();
    malformed.intensity = std::numeric_limits<float>::infinity();
    malformed.passes = 100;
    malformed.whitePoint = -2;
    const auto safe = Sanitize(malformed);
    assert(safe.scale == 1 && safe.intensity == 1 && safe.passes == 3 && safe.whitePoint == 0.01f);
    SetSettings(malformed);
    assert(GetSettings() == safe);
    assert(sizeof(DlssNrConstants) == 256);
    std::cout << "PASS: defaults, on/off, invalid values, INI round-trip and FG/plugin/shortcut preservation\n";
}
