// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ports/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace bluev
{

namespace
{

std::atomic<ILogSink*> s_sink{nullptr};

} // namespace

void setLogSink(ILogSink* sink)
{
    s_sink.store(sink);
}

ILogSink* logSink()
{
    return s_sink.load();
}

void flushLog()
{
    if (ILogSink* sink = s_sink.load())
    {
        sink->flush();
    }
}

#define BLUEV_FORMAT(method)                                                                                           \
    ILogSink* sink = s_sink.load();                                                                                    \
    if (sink == nullptr)                                                                                               \
        return;                                                                                                        \
    char text[512];                                                                                                    \
    va_list args;                                                                                                      \
    va_start(args, fmt);                                                                                               \
    std::vsnprintf(text, sizeof text, fmt, args);                                                                      \
    va_end(args);                                                                                                      \
    sink->method(text)

void logLine(const char* fmt, ...)
{
    BLUEV_FORMAT(line);
}

void notify(const char* fmt, ...)
{
    BLUEV_FORMAT(notify);
}

void notifyShow(const char* fmt, ...)
{
    BLUEV_FORMAT(notifyShow);
}

} // namespace bluev
