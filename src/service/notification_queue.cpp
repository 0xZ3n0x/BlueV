// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/notification_queue.hpp"

namespace bluev
{

void NotificationQueue::start()
{
    ScopedLock lock(m_mutex);

    if (m_running)
    {
        return;
    }
    m_stopping = false;
    m_running = true;
    m_thread = std::thread([this] { run(); });
}

void NotificationQueue::stop()
{
    {
        ScopedLock lock(m_mutex);

        if (!m_running)
        {
            return;
        }
        m_stopping = true;
        m_running = false;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void NotificationQueue::post(std::string text, bool hold)
{
    {
        ScopedLock lock(m_mutex);

        if (m_queue.size() >= kMaxQueued)
        {
            m_queue.pop_front();
        }
        m_queue.push_back(Item{std::move(text), hold});
    }
    m_wake.notify_one();
}

void NotificationQueue::run()
{
    for (;;)
    {
        Item item;

        {
            ScopedLock lock(m_mutex);

            waitFor(m_wake, m_mutex, [this]() BLUEV_REQUIRES(m_mutex) { return !m_queue.empty() || m_stopping; });
            if (m_queue.empty())
            {
                return; // stopping and nothing left
            }
            item = std::move(m_queue.front());
            m_queue.pop_front();
        }
        m_sender(item.text);
        if (item.hold)
        {
            // wake early when stopping, but never skip what is queued
            ScopedLock lock(m_mutex);

            waitFor(m_wake, m_mutex, m_holdTime, [this]() BLUEV_REQUIRES(m_mutex) { return m_stopping; });
        }
    }
}

} // namespace bluev
