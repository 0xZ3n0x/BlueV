// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/settings.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace bluev
{

Settings SettingsFile::load() const
{
    Settings settings;
    std::FILE* file = std::fopen(m_path.c_str(), "r");
    char line[128];

    if (file == nullptr)
    {
        return settings;
    }
    while (std::fgets(line, sizeof line, file) != nullptr)
    {
        unsigned flag = 0, vid = 0, pid = 0, quality = 0;
        char network[8] = {};

        if (std::sscanf(line, "autostart=%u", &flag) == 1)
        {
            settings.autostart = flag != 0;
        }
        else if (std::sscanf(line, "network=%7s", network) == 1)
        {
            settings.localOnly = std::strcmp(network, "local") == 0;
        }
        else if (std::sscanf(line, "quality=%u", &quality) == 1)
        {
            settings.quality = quality == 1 ? 1 : 0;
        }
        else if (std::sscanf(line, "adapter=%4x:%4x", &vid, &pid) == 2)
        {
            settings.vid = static_cast<std::uint16_t>(vid);
            settings.pid = static_cast<std::uint16_t>(pid);
        }
    }
    std::fclose(file);
    return settings;
}

bool SettingsFile::save(const Settings& settings) const
{
    // written beside the file and renamed over it, so a crash leaves the old one whole
    const std::string temporary = m_path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "w");

    if (file == nullptr)
    {
        return false;
    }
    std::fprintf(file, "autostart=%d\nadapter=%04x:%04x\nquality=%d\nnetwork=%s\n", settings.autostart ? 1 : 0,
                 settings.vid, settings.pid, settings.quality, settings.localOnly ? "local" : "lan");
    const bool written = std::fclose(file) == 0;

    return written && std::rename(temporary.c_str(), m_path.c_str()) == 0;
}

} // namespace bluev
