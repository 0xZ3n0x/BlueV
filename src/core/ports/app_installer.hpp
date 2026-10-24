// SPDX-License-Identifier: GPL-3.0-or-later
// The system installer behind the home screen entry (see IShortcut).
#pragma once

#include <string>

namespace bluev
{

// The system's app installer, which puts an entry on the console's home
// screen for a title directory.
class IAppInstaller
{
  public:
    virtual ~IAppInstaller() = default;
    // Registers the title `titleId` whose directory is below `root`.
    virtual bool registerTitle(const std::string& titleId, const std::string& root) = 0;
    // Removes the entry again. Both return false when the system refuses.
    virtual bool unregisterTitle(const std::string& titleId) = 0;
};

} // namespace bluev
