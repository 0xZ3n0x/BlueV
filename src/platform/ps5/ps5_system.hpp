// SPDX-License-Identifier: GPL-3.0-or-later
// The small pieces of the console that have no better home: the model name,
// the app installer and the on-screen notification.
#pragma once

#include <string>

#include "core/ports/app_installer.hpp"
#include "core/ports/system_info.hpp"

namespace bluev
{

class Ps5SystemInfo final : public ISystemInfo
{
  public:
    // From sceKernelGetHwModelName; empty when the call fails.
    std::string modelName() override;
};

// The system's app installer. Linking its stub library keeps the payload from
// starting at all on the console, so the library is loaded here and the
// functions are looked up by name, like the capture code does for libSceAvcap2.
class Ps5AppInstaller final : public IAppInstaller
{
  public:
    // Tells the installer to register the title; false, with the reason in the
    // log, when the library or the call fails.
    bool registerTitle(const std::string& titleId, const std::string& root) override;
    bool unregisterTitle(const std::string& titleId) override;

  private:
    struct Functions
    {
        int (*initialize)() = nullptr;
        int (*terminate)() = nullptr;
        int (*installTitleDir)(const char*, const char*, void*) = nullptr;
        int (*installAll)(void*) = nullptr;
        int (*uninstall)(const char*) = nullptr;
    };
    // Loads the library and looks the functions up; false when too few are found.
    bool load(Functions& f);
};

// a message on the console's screen
void sendNotification(const std::string& text);

} // namespace bluev
