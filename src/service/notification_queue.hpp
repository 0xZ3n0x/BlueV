// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen notifications, sent from a worker thread so that the thread that
// posts them (usually the radio thread) never sleeps. A notification that is
// meant to be read is held on screen for a moment before the next one goes
// out. The newest kMaxQueued are kept; older ones are dropped.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string>
#include <thread>

#include "service/sync.hpp"

namespace bluev
{

class NotificationQueue
{
  public:
    using Sender = std::function<void(const std::string&)>;

    explicit NotificationQueue(Sender sender, std::chrono::milliseconds holdTime = std::chrono::milliseconds(400))
        : m_sender(std::move(sender)), m_holdTime(holdTime)
    {
    }
    ~NotificationQueue()
    {
        stop();
    }
    NotificationQueue(const NotificationQueue&) = delete;
    NotificationQueue& operator=(const NotificationQueue&) = delete;

    // Starts the worker. Posts made before it are kept and sent then.
    void start() BLUEV_EXCLUDES(m_mutex);
    // Sends what is queued (without holding), then stops the worker.
    void stop() BLUEV_EXCLUDES(m_mutex);

    // `hold` keeps the notification visible before the next one is sent.
    void post(std::string text, bool hold) BLUEV_EXCLUDES(m_mutex);

  private:
    struct Item
    {
        std::string text;
        bool hold;
    };

    void run() BLUEV_EXCLUDES(m_mutex);

    static constexpr std::size_t kMaxQueued = 32;

    Sender m_sender;
    std::chrono::milliseconds m_holdTime;
    Mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Item> m_queue BLUEV_GUARDED_BY(m_mutex);
    bool m_stopping BLUEV_GUARDED_BY(m_mutex) = false;
    bool m_running BLUEV_GUARDED_BY(m_mutex) = false;
    std::thread m_thread;
};

} // namespace bluev
