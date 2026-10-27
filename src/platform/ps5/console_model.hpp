// SPDX-License-Identifier: GPL-3.0-or-later
// Which console model this is, for the log.
#pragma once

#include <string>

namespace bluev
{

// From sceKernelGetHwModelName, e.g. "CFI-2002 B01"; empty when the call fails.
std::string consoleModelName();

} // namespace bluev
