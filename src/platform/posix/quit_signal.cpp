// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/posix/quit_signal.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <unistd.h>

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

const char* g_crashPath = nullptr;

// Only async-signal-safe calls: open, write, close, signal, raise.
void onFatalSignal(int number)
{
    const int fd = g_crashPath != nullptr ? open(g_crashPath, O_WRONLY | O_APPEND | O_CREAT, 0644) : -1;

    if (fd >= 0)
    {
        char text[] = "FATAL: ended by signal 00\n";
        const int tens = number / 10 % 10, ones = number % 10;

        text[sizeof text - 4] = static_cast<char>('0' + tens);
        text[sizeof text - 3] = static_cast<char>('0' + ones);
        (void)!write(fd, text, sizeof text - 1);
        close(fd);
    }
    std::signal(number, SIG_DFL);
    raise(number);
}

} // namespace

void installCrashTrace(const char* path)
{
    g_crashPath = path;
    for (const int number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGHUP, SIGINT, SIGQUIT, SIGUSR1})
    {
        std::signal(number, onFatalSignal);
    }
}

void installQuitSignalHandling()
{
    std::signal(SIGTERM, onSigTerm);
}

bool takeQuitRequestedFlag()
{
    return g_quitRequested.exchange(false, std::memory_order_relaxed);
}

} // namespace bluev
