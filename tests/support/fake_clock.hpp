// SPDX-License-Identifier: GPL-3.0-or-later
// A clock that only moves when a test (or the fake transport) says so.
#pragma once

#include <cstdint>

#include "core/ports/clock.hpp"

namespace bluev::testing
{

class FakeClock final : public IClock
{
  public:
    std::uint64_t nowMs() override
    {
        return m_now;
    }
    void advance(std::uint64_t ms)
    {
        m_now += ms;
    }
    void set(std::uint64_t ms)
    {
        m_now = ms;
    }

  private:
    std::uint64_t m_now = 100000; // not near zero, like a real uptime
};

} // namespace bluev::testing
