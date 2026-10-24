// SPDX-License-Identifier: GPL-3.0-or-later
// What the console can tell about itself, for the first log lines.
#pragma once

#include <string>

namespace bluev
{

class ISystemInfo
{
  public:
    virtual ~ISystemInfo() = default;
    virtual std::string modelName() = 0; // e.g. "CFI-2002 B01", empty when unknown
};

} // namespace bluev
