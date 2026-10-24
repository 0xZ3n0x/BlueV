// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/home_screen_shortcut.hpp"

#include <cerrno>
#include <cstdio>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/ports/log.hpp"

namespace bluev
{

namespace
{

std::string readFile(const std::string& path)
{
    std::string data;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    char chunk[4096];
    std::size_t got;

    if (file == nullptr)
    {
        return data;
    }
    while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0)
    {
        data.append(chunk, got);
    }
    std::fclose(file);
    return data;
}

bool writeFile(const std::string& path, const std::string& data)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");

    if (file == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    return std::fclose(file) == 0 && ok;
}

bool makeDirectories(const std::string& path)
{
    std::string partial;

    for (std::size_t i = 0; i <= path.size(); i++)
    {
        if (i == path.size() || path[i] == '/')
        {
            if (!partial.empty() && mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
            {
                return false;
            }
        }
        if (i < path.size())
        {
            partial += path[i];
        }
    }
    return true;
}

} // namespace

std::string HomeScreenShortcut::paramJson(int port)
{
    char text[512];

    std::snprintf(text, sizeof text,
                  "{\n"
                  "    \"titleId\": \"%s\",\n"
                  "    \"applicationCategoryType\": 65536,\n"
                  "    \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
                  "    \"localizedParameters\": {\n"
                  "        \"defaultLanguage\": \"en-US\",\n"
                  "        \"en-US\": {\n"
                  "            \"titleName\": \"BlueV\"\n"
                  "        }\n"
                  "    }\n"
                  "}\n",
                  kTitleId, port);
    return text;
}

bool HomeScreenShortcut::install(int port)
{
    const std::string base = m_root + "/" + kTitleId;
    const std::string sceSys = base + "/sce_sys";
    const std::string paramPath = sceSys + "/param.json";
    const std::string iconPath = sceSys + "/icon0.png";
    const std::string param = paramJson(port);
    struct stat info;

    // The marker says the system's installer accepted the directory: files
    // that are right but were never registered (the installer failed the time
    // before) must be tried again.
    const std::string marker = base + "/.registered";
    const bool present = stat(base.c_str(), &info) == 0;
    if (present && stat(marker.c_str(), &info) == 0 && readFile(paramPath) == param && readFile(iconPath) == m_icon)
    {
        return true; // already as it should be
    }
    unlink(marker.c_str());

    logLine("shortcut: %s the home screen entry %s", present ? "updating" : "installing", kTitleId);
    if (!makeDirectories(sceSys) || !writeFile(paramPath, param) || !writeFile(iconPath, m_icon))
    {
        logLine("shortcut: cannot write below %s (errno %d)", base.c_str(), errno);
        return false;
    }
    if (!m_installer.registerTitle(kTitleId, m_root + "/"))
    {
        return false;
    }
    writeFile(marker, "registered\n");
    logLine("shortcut: the home screen entry opens http://127.0.0.1:%d/", port);
    notify("BlueV: home screen entry ready");
    return true;
}

bool HomeScreenShortcut::remove()
{
    const std::string base = m_root + "/" + kTitleId;
    const bool ok = m_installer.unregisterTitle(kTitleId);

    unlink((base + "/.registered").c_str());
    unlink((base + "/sce_sys/param.json").c_str());
    unlink((base + "/sce_sys/icon0.png").c_str());
    rmdir((base + "/sce_sys").c_str());
    rmdir(base.c_str());
    if (ok)
    {
        logLine("shortcut: removed the home screen entry %s", kTitleId);
    }
    return ok;
}

} // namespace bluev
