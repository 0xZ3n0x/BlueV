// SPDX-License-Identifier: GPL-3.0-or-later
// The home screen entry as the service sees it; see IAppInstaller for the system side.
#pragma once

namespace bluev
{

// The console's home screen entry that opens the web interface.
class IShortcut
{
  public:
    virtual ~IShortcut() = default;
    // Creates or refreshes the entry so it opens http://127.0.0.1:<port>/.
    virtual bool install(int port) = 0;
    // Removes it; true also when there was none.
    virtual bool remove() = 0;
};

} // namespace bluev
