// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/avcap_library.hpp"

#include <cstring>
#include <unistd.h>

#include "core/ports/log.hpp"
#include "platform/ps5/ps5_api.hpp"

namespace bluev
{

bool AvcapLibrary::load()
{
    int unused = 0;

    int h = sceKernelLoadStartModule("/system/common/lib/libSceIpmi.sprx", 0, nullptr, 0, nullptr, &unused);
    if (h < 0)
    {
        logLine("capture: load %s -> %#x", "/system/common/lib/libSceIpmi.sprx", h);
        return false;
    }
    h = sceKernelLoadStartModule("/system/common/lib/libSceAvcap2.sprx", 0, nullptr, 0, nullptr, &unused);
    if (h < 0)
    {
        logLine("capture: load %s -> %#x", "/system/common/lib/libSceAvcap2.sprx", h);
        return false;
    }

    struct Entry
    {
        const char* nid;
        const char* name;
        void** slot;
    };
    const Entry table[] = {
        {"svzPXluOz8U", "sceAvcap2Initialize", reinterpret_cast<void**>(&m_initialize)},
        {"gojqghDU+1Y", "sceAvcap2Terminate", reinterpret_cast<void**>(&m_terminate)},
        {"3nHpE7Dp5SE", "sceAvcap2OpenAudio", reinterpret_cast<void**>(&m_openAudio)},
        {"tR7gPe1i8hw", "sceAvcap2Close", reinterpret_cast<void**>(&m_close)},
        {"8CGNCwBsItI", "sceAvcap2Start", reinterpret_cast<void**>(&m_start)},
        {"Z5dKK0xnQ8g", "sceAvcap2Stop", reinterpret_cast<void**>(&m_stop)},
        {"WhsHggqdwcg", "sceAvcap2ReadAudio", reinterpret_cast<void**>(&m_readAudio)},
    };
    for (const Entry& e : table)
    {
        *e.slot = reinterpret_cast<void*>(kernel_dynlib_resolve(getpid(), static_cast<uint32_t>(h), e.nid));
        if (*e.slot == nullptr)
        {
            logLine("capture: %s (%s) not found", e.name, e.nid);
        }
    }
    for (const Entry& e : table)
    {
        if (*e.slot == nullptr)
        {
            return false;
        }
    }
    return true;
}

int AvcapLibrary::initialize()
{
    return m_initialize();
}

int AvcapLibrary::terminate()
{
    return m_terminate();
}

int AvcapLibrary::openAudio(void** handle)
{
    struct
    {
        std::uint64_t size;    // 0x38
        std::uint64_t version; // 0x18
        std::uint32_t pad0;
        std::uint32_t channels; // 2
        std::uint32_t pad1;
        std::uint32_t device; // 0xffffffff
        std::uint32_t pad2[6];
    } param;

    std::memset(&param, 0, sizeof param);
    param.size = 0x38;
    param.version = 0x18;
    param.channels = 2;
    param.device = 0xffffffff;
    return m_openAudio(handle, &param, 0);
}

int AvcapLibrary::close(void* handle)
{
    return m_close(handle);
}

int AvcapLibrary::start(void* handle)
{
    return m_start(handle);
}

int AvcapLibrary::stop(void* handle)
{
    return m_stop(handle);
}

int AvcapLibrary::readAudio(void* handle, float* buffer, std::uint32_t maxBytes)
{
    std::uint8_t meta[16];

    return m_readAudio(handle, buffer, maxBytes, meta, 1);
}

} // namespace bluev
