// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/avcap_source.hpp"

#include <cstring>
#include <unistd.h>

#include "core/ports/log.hpp"
#include "platform/ps5/ps5_api.hpp"

namespace bluev
{

int AvcapSource::open()
{
    int unused = 0;

    int h = sceKernelLoadStartModule("/system/common/lib/libSceIpmi.sprx", 0, nullptr, 0, nullptr, &unused);
    if (h < 0)
    {
        logLine("capture: load %s -> %#x", "/system/common/lib/libSceIpmi.sprx", h);
        return -1;
    }
    h = sceKernelLoadStartModule("/system/common/lib/libSceAvcap2.sprx", 0, nullptr, 0, nullptr, &unused);
    if (h < 0)
    {
        logLine("capture: load %s -> %#x", "/system/common/lib/libSceAvcap2.sprx", h);
        return -1;
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
            return -1;
        }
    }
    return start() ? 1 : 0;
}

void AvcapSource::teardown()
{
    if (m_started)
    {
        m_stop(m_handle);
    }
    if (m_handle != nullptr)
    {
        m_close(m_handle);
    }
    if (m_initialized)
    {
        m_terminate();
    }
    m_initialized = false;
    m_started = false;
    m_handle = nullptr;
}

bool AvcapSource::failStep(const char* what, int error)
{
    if (error != m_lastError)
    {
        logLine("capture: %s -> %#x, not available yet", what, error);
    }
    m_lastError = error;
    teardown();
    return false;
}

bool AvcapSource::start()
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

    int err = m_initialize();
    if (err < 0)
    {
        return failStep("Initialize", err);
    }
    m_initialized = true;

    std::memset(&param, 0, sizeof param);
    param.size = 0x38;
    param.version = 0x18;
    param.channels = 2;
    param.device = 0xffffffff;
    err = m_openAudio(&m_handle, &param, 0);
    if (err < 0 || m_handle == nullptr)
    {
        m_handle = nullptr;
        return failStep("OpenAudio", err);
    }

    err = m_start(m_handle);
    if (err < 0)
    {
        return failStep("Start", err);
    }

    m_started = true;
    m_lastError = 0;
    logLine("capture: running, %d Hz stereo", 48000);
    return true;
}

int AvcapSource::read(float* buffer, std::uint32_t maxBytes)
{
    std::uint8_t meta[16];

    if (!m_started)
    {
        return -1;
    }
    const int r = m_readAudio(m_handle, buffer, maxBytes, meta, 1);
    if (r == static_cast<int>(0x81950002u)) // nothing new yet
    {
        return 0;
    }
    if (r == static_cast<int>(0x81950004u))
    { // ring overrun: skipped to live
        if (m_overruns < 5)
        {
            logLine("capture: ring overrun, skipped to live audio");
        }
        m_overruns++;
        return 0;
    }
    if (r < 0)
    {
        logLine("capture: ReadAudio -> %#x", r);
        return -1;
    }
    return r;
}

bool AvcapSource::restart()
{
    teardown();
    return start();
}

void AvcapSource::close()
{
    if (m_initialize != nullptr) // only when open() got as far as resolving
    {
        teardown();
    }
}

} // namespace bluev
