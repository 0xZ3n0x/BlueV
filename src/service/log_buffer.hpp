// SPDX-License-Identifier: GPL-3.0-or-later
// The recent log lines in a ring (served by the web page) and the lines not
// yet written to the file.
#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "core/ports/log_reader.hpp"
#include "service/sync.hpp"

namespace bluev
{

class LogBuffer final : public ILogReader
{
  public:
    // a finished line, with its time stamp, without the newline
    void add(const std::string& line) BLUEV_EXCLUDES(m_mutex);
    // what was added since the last call, newline terminated
    std::string takePending() BLUEV_EXCLUDES(m_mutex);
    // Lines numbered from `from` on (the first line is number 0), at most `maxBytes`
    // of text; `next` is what to ask for the next time. Only the last kRingLines
    // lines exist.
    std::string since(unsigned from, unsigned& next, std::size_t maxBytes) override BLUEV_EXCLUDES(m_mutex);

    static constexpr std::size_t kRingLines = 300;
    static constexpr std::size_t kLineLength = 200;
    static constexpr std::size_t kPendingLimit = 64 * 1024;

  private:
    using Line = std::array<char, kLineLength>;

    Mutex m_mutex;
    std::array<Line, kRingLines> m_ring BLUEV_GUARDED_BY(m_mutex){};
    unsigned m_sequence BLUEV_GUARDED_BY(m_mutex) = 0; // lines logged so far
    std::string m_pending BLUEV_GUARDED_BY(m_mutex);   // not yet in the file
};

} // namespace bluev
