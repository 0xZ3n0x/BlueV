// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/avcap_capture.hpp"

#include "core/ports/log.hpp"

namespace bluev
{

namespace
{
constexpr int kNothingNewYet = static_cast<int>(0x81950002u);
constexpr int kRingOverrun = static_cast<int>(0x81950004u); // the capture ran ahead and skipped to live audio
constexpr long kOverrunsLogged = 5;
} // namespace

void AvcapCapture::teardown()
{
    if (m_started)
    {
        m_library.stop(m_handle);
    }
    if (m_handle != nullptr)
    {
        m_library.close(m_handle);
    }
    if (m_initialized)
    {
        m_library.terminate();
    }
    m_initialized = false;
    m_started = false;
    m_handle = nullptr;
}

bool AvcapCapture::failStep(const char* what, int error)
{
    if (error != m_lastError)
    {
        logLine("capture: %s -> %#x, not available yet", what, error);
    }
    m_lastError = error;
    teardown();
    return false;
}

bool AvcapCapture::start()
{
    int err = m_library.initialize();

    if (err < 0)
    {
        return failStep("Initialize", err);
    }
    m_initialized = true;

    err = m_library.openAudio(&m_handle);
    if (err < 0 || m_handle == nullptr)
    {
        m_handle = nullptr;
        return failStep("OpenAudio", err);
    }

    err = m_library.start(m_handle);
    if (err < 0)
    {
        return failStep("Start", err);
    }

    m_started = true;
    m_lastError = 0;
    logLine("capture: running, %d Hz stereo", 48000);
    return true;
}

int AvcapCapture::read(float* buffer, std::uint32_t maxBytes)
{
    if (!m_started)
    {
        return -1;
    }
    const int r = m_library.readAudio(m_handle, buffer, maxBytes);

    if (r == kNothingNewYet)
    {
        return 0;
    }
    if (r == kRingOverrun)
    {
        if (m_overruns < kOverrunsLogged)
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

bool AvcapCapture::restart()
{
    teardown();
    return start();
}

void AvcapCapture::close()
{
    teardown();
}

} // namespace bluev
