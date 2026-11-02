// SPDX-License-Identifier: GPL-3.0-or-later
// The program's log sink: stamps each line, keeps it in the ring buffer for
// the web page and writes the file on flush(). Notifications go to the sender.
#pragma once

#include <ctime>
#include <functional>
#include <string>

#include "core/ports/log.hpp"
#include "service/log_buffer.hpp"

namespace bluev
{

class AppLog final : public ILogSink
{
  public:
    using Writer = std::function<void(const std::string&)>;

    // `file`: gets what accumulated, from flush(); may be empty.
    // `notifier`: puts a message on the console's screen; may be empty.
    AppLog(
        LogBuffer& buffer, Writer file, Writer notifier,
        std::function<std::time_t()> now = [] { return std::time(nullptr); })
        : m_buffer(buffer), m_file(std::move(file)), m_notifier(std::move(notifier)), m_now(std::move(now))
    {
    }

    void line(const char* text) override;
    void notify(const char* text) override;
    void flush() override;

  private:
    LogBuffer& m_buffer;
    Writer m_file;
    Writer m_notifier;
    std::function<std::time_t()> m_now;
};

} // namespace bluev
