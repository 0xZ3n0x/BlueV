// SPDX-License-Identifier: GPL-3.0-or-later
// Read side of the log, separate from ILogSink (the write side).
#pragma once

#include <cstddef>
#include <string>

namespace bluev
{

// The recent log lines, for the web page.
class ILogReader
{
  public:
    virtual ~ILogReader() = default;
    // Lines numbered >= `from`, newline separated; `next` is the number to
    // ask for the next time. At most `maxBytes` are returned.
    virtual std::string since(unsigned from, unsigned& next, std::size_t maxBytes) = 0;
};

} // namespace bluev
