// SPDX-License-Identifier: GPL-3.0-or-later
// The time source. The stack asks it for the time and never sleeps by itself;
// a test replaces it with a clock it moves by hand.
#pragma once

#include <cstdint>

namespace bluev
{

class IClock
{
  public:
    virtual ~IClock() = default;
    virtual std::uint64_t nowMs() = 0; // monotonic milliseconds
};

// steady_clock
class SystemClock final : public IClock
{
  public:
    std::uint64_t nowMs() override;
};

} // namespace bluev
