// SPDX-License-Identifier: GPL-3.0-or-later
// A log sink that keeps what was written, for assertions.
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "core/ports/log.hpp"

namespace bluev::testing
{

class CaptureSink final : public ILogSink
{
  public:
    CaptureSink()
    {
        setLogSink(this);
    }
    ~CaptureSink() override
    {
        setLogSink(nullptr);
    }

    void line(const char* text) override
    {
        add(m_lines, text);
    }
    void notify(const char* text) override
    {
        add(m_notes, text);
    }
    void notifyShow(const char* text) override
    {
        add(m_notes, text);
        add(m_lines, text);
    }

    std::vector<std::string> lines() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_lines;
    }
    std::vector<std::string> notes() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_notes;
    }

    // is there a line that contains `part`?
    bool has(const std::string& part) const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (const std::string& s : m_lines)
        {
            if (s.find(part) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
    int count(const std::string& part) const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        int n = 0;
        for (const std::string& s : m_lines)
        {
            if (s.find(part) != std::string::npos)
            {
                n++;
            }
        }
        return n;
    }
    std::string all() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        std::string out;
        for (const std::string& s : m_lines)
        {
            out += s + "\n";
        }
        return out;
    }

  private:
    void add(std::vector<std::string>& to, const char* text)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        to.emplace_back(text);
    }

    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines, m_notes;
};

} // namespace bluev::testing
