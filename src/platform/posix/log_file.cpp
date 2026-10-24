// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/posix/log_file.hpp"

#include <sys/stat.h>

namespace bluev
{

bool LogFile::open(const std::string& directory, const std::string& path)
{
    mkdir(directory.c_str(), 0755);
    std::FILE* f = std::fopen(path.c_str(), "ab");
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_file != nullptr)
    {
        std::fclose(m_file);
    }
    m_file = f;
    m_path = path;
    m_size = 0;
    if (m_file != nullptr)
    {
        std::fseek(m_file, 0, SEEK_END);
        const long size = std::ftell(m_file);

        m_size = size > 0 ? static_cast<std::size_t>(size) : 0;
        if (m_size >= m_maxBytes)
        {
            rotate();
        }
    }
    return m_file != nullptr;
}

void LogFile::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_file != nullptr)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void LogFile::append(const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_file != nullptr)
    {
        std::fwrite(text.data(), 1, text.size(), m_file);
        std::fflush(m_file);
        m_size += text.size();
        if (m_size >= m_maxBytes)
        {
            rotate();
        }
    }
}

void LogFile::rotate()
{
    std::fclose(m_file);
    const bool moved = std::rename(m_path.c_str(), (m_path + ".1").c_str()) == 0;

    m_file = std::fopen(m_path.c_str(), moved ? "ab" : "wb");
    m_size = 0;
}

} // namespace bluev
