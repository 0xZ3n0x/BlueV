// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/app_log.hpp"

#include <cstring>

namespace bluev
{

// The stamp is UTC: the console has no time zone for a payload to ask.
void AppLog::line(const char* text)
{
    char stamp[32];
    const std::time_t now = m_now();
    std::tm parts{};

    gmtime_r(&now, &parts);
    if (std::strftime(stamp, sizeof stamp, "%H:%M:%S", &parts) == 0)
    {
        std::strcpy(stamp, "00:00:00");
    }
    const std::string stamped = std::string(stamp) + " " + text;

    m_buffer.add(stamped);
}

void AppLog::notify(const char* text)
{
    if (m_notifier)
    {
        m_notifier(text);
    }
}

void AppLog::flush()
{
    const std::string out = m_buffer.takePending();

    if (!out.empty() && m_file)
    {
        m_file(out);
    }
}

} // namespace bluev
