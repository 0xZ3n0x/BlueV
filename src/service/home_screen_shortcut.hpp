// SPDX-License-Identifier: GPL-3.0-or-later
// The console's home screen entry: an "app" whose only content is a
// param.json that sends the console's browser to our web interface (the
// deeplink), plus an icon. This is the same mechanism the payload manager
// uses; the system's app installer registers the directory.
#pragma once

#include <string>
#include <string_view>

#include "core/ports/app_installer.hpp"

namespace bluev
{

class HomeScreenShortcut
{
  public:
    static constexpr const char* kTitleId = "BLUV00001";
    static constexpr const char* kAppRoot = "/user/app";

    // `root` is where the title directory lives (the console's /user/app)
    HomeScreenShortcut(IAppInstaller& installer, std::string_view icon, std::string root = kAppRoot)
        : m_installer(installer), m_icon(icon), m_root(std::move(root))
    {
    }

    // Makes sure the entry exists and points at `port`. Nothing is done when
    // the files are already as they should be and the installer accepted them
    // before. False when the files cannot be written or the installer fails.
    bool install(int port);
    // Unregisters the entry and deletes its files. The result is the installer's:
    // false when the system did not accept the removal (the files go anyway).
    bool remove();

    // what goes into param.json for `port`
    static std::string paramJson(int port);

  private:
    IAppInstaller& m_installer;
    std::string m_icon;
    std::string m_root;
};

} // namespace bluev
