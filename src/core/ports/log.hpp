// SPDX-License-Identifier: GPL-3.0-or-later
// The log and the on-screen notifications. One sink for the whole program
// (set once at start); without a sink everything is dropped.
#pragma once

namespace bluev
{

// Where log lines and notifications go. Calls can come from any thread.
class ILogSink
{
  public:
    virtual ~ILogSink() = default;
    virtual void line(const char* text) = 0;       // one line of the log
    virtual void notify(const char* text) = 0;     // an on-screen message
    virtual void notifyShow(const char* text) = 0; // the same, and logged
    virtual void flush()
    {
    } // write what is buffered (about once a second)
};

// Installs the sink; the caller keeps it alive. nullptr drops everything.
void setLogSink(ILogSink* sink);
ILogSink* logSink();

// ILogSink::flush() of the current sink
void flushLog();

// printf style; text longer than 511 characters is cut. Each line is stamped by the sink. notify() shows a message on
// the screen only, notifyShow() also writes it to the log.
void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void notifyShow(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace bluev
