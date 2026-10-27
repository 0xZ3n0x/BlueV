// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/posix/quit_signal.hpp"

#include <atomic>
#include <csignal>

namespace bluev
{

namespace
{

std::atomic<bool> g_quitRequested{false};

void onSigTerm(int /*signal*/)
{
    // async-signal-safe only: set the flag and return, nothing else
    g_quitRequested.store(true, std::memory_order_relaxed);
}

} // namespace

void installQuitSignalHandling()
{
    std::signal(SIGTERM, onSigTerm);
}

bool takeQuitRequestedFlag()
{
    return g_quitRequested.exchange(false, std::memory_order_relaxed);
}

} // namespace bluev
