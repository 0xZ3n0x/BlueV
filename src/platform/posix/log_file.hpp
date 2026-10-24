// SPDX-License-Identifier: GPL-3.0-or-later
// The log file: appended to by flush(), never from the logging threads. It
// stays small: when it reaches a size limit it becomes `<path>.1` (replacing
// the one before) and a new file is started, so at most two files exist.
#pragma once

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>

namespace bluev
{

class LogFile
{
  public:
    // The size at which the file is turned over; the two files together stay below twice this.
    static constexpr std::size_t kDefaultMaxBytes = 512 * 1024;

    explicit LogFile(std::size_t maxBytes = kDefaultMaxBytes) : m_maxBytes(maxBytes)
    {
    }
    ~LogFile()
    {
        close();
    }

    // Opens `path` for appending, creating `directory` first when it does not
    // exist; false when the file cannot be opened. Replaces an earlier file. A
    // file that is already at the size limit is turned over first.
    bool open(const std::string& directory, const std::string& path);
    void close();
    // Writes and flushes at once, then turns the file over when it has reached
    // the limit; does nothing when no file is open. Thread safe.
    void append(const std::string& text);

  private:
    // Moves the file to `<path>.1` and starts a new one. When the move fails the
    // file is emptied instead, so the size stays bounded. The caller holds the lock.
    void rotate();

    std::mutex m_mutex;
    std::FILE* m_file = nullptr;
    std::string m_path;
    std::size_t m_size = 0; // bytes in the open file
    std::size_t m_maxBytes;
};

} // namespace bluev
