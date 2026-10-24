// SPDX-License-Identifier: GPL-3.0-or-later
// A directory that is removed with the object.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace bluev::testing
{

class TempDir
{
  public:
    TempDir()
    {
        char pattern[] = "/tmp/bluev-test-XXXXXX";
        m_path = mkdtemp(pattern);
    }
    ~TempDir()
    {
        std::filesystem::remove_all(m_path);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const
    {
        return m_path;
    }
    std::string file(const std::string& name) const
    {
        return m_path + "/" + name;
    }
    bool exists(const std::string& name) const
    {
        return std::filesystem::exists(file(name));
    }

  private:
    std::string m_path;
};

} // namespace bluev::testing
