#include "Settings.h"
#include <mutex>

namespace DlssNr
{
namespace
{
std::mutex settingsMutex;
Settings settings;
} // namespace
Settings GetSettings()
{
    std::lock_guard lock(settingsMutex);
    return settings;
}
void SetSettings(Settings value)
{
    std::lock_guard lock(settingsMutex);
    settings = Sanitize(value);
}
} // namespace DlssNr
