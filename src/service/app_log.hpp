// SPDX-License-Identifier: GPL-3.0-or-later
// The program's log sink: stamps each line, keeps it in the ring buffer for
// the web page and writes the file on flush(). Notifications go to the queue.
#pragma once

#include <ctime>
#include <functional>
#include <string>

#include "core/ports/log.hpp"
#include "service/log_buffer.hpp"
#include "service/notification_queue.hpp"

namespace bluev
{

class AppLog final : public ILogSink
{
  public:
    using Writer = std::function<void(const std::string&)>;

    // `file`: gets what accumulated, from flush(); may be empty.
    AppLog(
        LogBuffer& buffer, NotificationQueue& notifications, Writer file,
        std::function<std::time_t()> now = [] { return std::time(nullptr); })
        : m_buffer(buffer), m_notifications(notifications), m_file(std::move(file)), m_now(std::move(now))
    {
    }

    void line(const char* text) override;
    void notify(const char* text) override;
    void notifyShow(const char* text) override;
    void flush() override;

  private:
    LogBuffer& m_buffer;
    NotificationQueue& m_notifications;
    Writer m_file;
    std::function<std::time_t()> m_now;
};

} // namespace bluev
