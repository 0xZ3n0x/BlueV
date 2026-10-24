// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/log_buffer.hpp"

#include <cstdio>
#include <cstring>

namespace bluev
{

// Lines longer than kLineLength - 1 are cut in the ring, not in the file text.
// Once kPendingLimit bytes wait for the file, further lines are kept only in
// the ring.
void LogBuffer::add(const std::string& line)
{
    ScopedLock lock(m_mutex);

    std::snprintf(m_ring[m_sequence % kRingLines].data(), kLineLength, "%s", line.c_str());
    m_sequence++;
    if (m_pending.size() < kPendingLimit)
    {
        m_pending += line;
        m_pending += '\n';
    }
}

std::string LogBuffer::takePending()
{
    std::string out;
    ScopedLock lock(m_mutex);

    out.swap(m_pending);
    return out;
}

std::string LogBuffer::since(unsigned from, unsigned& next, std::size_t maxBytes)
{
    std::string out;
    ScopedLock lock(m_mutex);

    // `from` ahead of what exists: the caller saw an earlier run of the program
    // (the numbers start again at 0), so it gets everything that is kept
    if (from > m_sequence)
    {
        from = 0;
    }
    if (m_sequence - from > kRingLines) // older lines are gone from the ring
    {
        from = m_sequence - static_cast<unsigned>(kRingLines);
    }
    for (unsigned i = from; i != m_sequence; i++)
    {
        const char* line = m_ring[i % kRingLines].data();
        const std::size_t length = std::strlen(line);

        if (out.size() + length + 1 > maxBytes)
        {
            break;
        }
        out.append(line, length);
        out += '\n';
        from = i + 1;
    }
    next = from;
    return out;
}

} // namespace bluev
