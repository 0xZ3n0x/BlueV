// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/posix/file_firmware_store.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace bluev
{

namespace
{
// nothing legitimate is this large; a bigger file is a mistake, not firmware
constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;
} // namespace

bool FileFirmwareStore::read(const std::string& name, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos)
    {
        return false;
    }

    const std::string path = m_dir + "/" + name;
    const int fd = ::open(path.c_str(), O_RDONLY);

    if (fd < 0)
    {
        return false;
    }

    std::uint8_t buffer[4096];
    bool ok = true;
    for (;;)
    {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);

        if (n < 0)
        {
            ok = false;
            break;
        }
        if (n == 0)
        {
            break;
        }
        out.insert(out.end(), buffer, buffer + n);
        if (out.size() > kMaxBytes)
        {
            ok = false;
            break;
        }
    }
    ::close(fd);
    if (!ok)
    {
        out.clear();
    }
    return ok && !out.empty();
}

} // namespace bluev
