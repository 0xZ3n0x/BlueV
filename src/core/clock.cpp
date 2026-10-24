// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ports/clock.hpp"

#include <chrono>

namespace bluev
{

std::uint64_t SystemClock::nowMs()
{
    using namespace std::chrono;

    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace bluev
