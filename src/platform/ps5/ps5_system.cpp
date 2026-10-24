// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/ps5_system.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/ports/log.hpp"
#include "platform/ps5/ps5_api.hpp"

namespace bluev
{

std::string Ps5SystemInfo::modelName()
{
    char model[1000] = "";

    if (sceKernelGetHwModelName(model) != 0)
    {
        model[0] = 0;
    }
    return model;
}

bool Ps5AppInstaller::load(Functions& f)
{
    constexpr const char* kModule = "libSceAppInstUtil.sprx";
    int unused = 0;
    uint32_t handle = 0;

    const int loaded =
        sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx", 0, nullptr, 0, nullptr, &unused);
    const int found = kernel_dynlib_handle(-1, kModule, &handle);

    logLine("shortcut: module load %#x, handle lookup %d (%u)", loaded, found, handle);
    if (found != 0)
    {
        return false;
    }
    f.initialize = reinterpret_cast<int (*)()>(kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilInitialize"));
    f.terminate = reinterpret_cast<int (*)()>(kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilTerminate"));
    // This one is not found by name on the console; the payload manager asks
    // for it by its NID.
    f.installTitleDir =
        reinterpret_cast<int (*)(const char*, const char*, void*)>(kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
    // The fallback of the payload manager, for firmware where the function
    // above is not there: register every new title of /user/app.
    f.installAll = reinterpret_cast<int (*)(void*)>(kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppInstallAll"));
    f.uninstall = reinterpret_cast<int (*)(const char*)>(kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppUnInstall"));
    logLine("shortcut: functions %p %p title-dir %p all %p %p", reinterpret_cast<void*>(f.initialize),
            reinterpret_cast<void*>(f.terminate), reinterpret_cast<void*>(f.installTitleDir),
            reinterpret_cast<void*>(f.installAll), reinterpret_cast<void*>(f.uninstall));
    return f.initialize && f.terminate && f.uninstall && (f.installTitleDir || f.installAll);
}

bool Ps5AppInstaller::registerTitle(const std::string& titleId, const std::string& root)
{
    Functions f;

    if (!load(f))
    {
        logLine("shortcut: the app installer cannot be loaded");
        return false;
    }
    int status = f.initialize();
    if (status != 0)
    {
        logLine("shortcut: app installer unavailable (%#x)", status);
        return false;
    }
    status = f.installTitleDir ? f.installTitleDir(titleId.c_str(), root.c_str(), nullptr) : f.installAll(nullptr);
    f.terminate();
    if (status != 0)
    {
        logLine("shortcut: install failed (%#x)", status);
        return false;
    }
    return true;
}

bool Ps5AppInstaller::unregisterTitle(const std::string& titleId)
{
    Functions f;
    bool ok = false;

    if (!load(f))
    {
        logLine("shortcut: the app installer cannot be loaded");
    }
    else if (const int status = f.initialize(); status != 0)
    {
        logLine("shortcut: app installer unavailable (%#x)", status);
    }
    else
    {
        const int result = f.uninstall(titleId.c_str());

        f.terminate();
        ok = result == 0;
        if (!ok)
        {
            logLine("shortcut: uninstall failed (%#x)", result);
        }
    }
    return ok;
}

namespace
{

// A "notification with user message" request: 45 bytes the system does not
// need, then the UTF-8 text (up to 3075 bytes with its NUL). The same layout
// as the notify_request_t of the SDK's notify_debug sample.
struct NotifyRequest
{
    char unused[45];
    char message[3075];
};

} // namespace

void sendNotification(const std::string& text)
{
    NotifyRequest request;

    std::memset(&request, 0, sizeof request);
    std::snprintf(request.message, sizeof request.message, "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

} // namespace bluev
