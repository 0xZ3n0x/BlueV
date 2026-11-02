// SPDX-License-Identifier: GPL-3.0-or-later
// The recent log lines in a ring (served by the web page) and the lines not
// yet written to the file.
#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <string>

namespace bluev
{

class LogBuffer
{
  public:
    // a finished line, with its time stamp, without the newline
    void add(const std::string& line);
    // what was added since the last call, newline terminated
    std::string takePending();
    // Lines numbered from `from` on (the first line is number 0), at most `maxBytes`
    // of text; `next` is what to ask for the next time. Only the last kRingLines
    // lines exist.
    std::string since(unsigned from, unsigned& next, std::size_t maxBytes);

    static constexpr std::size_t kRingLines = 300;
    static constexpr std::size_t kLineLength = 200;
    static constexpr std::size_t kPendingLimit = 64 * 1024;

  private:
    using Line = std::array<char, kLineLength>;

    std::mutex m_mutex;
    std::array<Line, kRingLines> m_ring{};
    unsigned m_sequence = 0; // lines logged so far
    std::string m_pending;   // not yet in the file
};

} // namespace bluev
