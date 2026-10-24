// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio mix through libSceAvcap2 (what Remote Play uses). The
// library is loaded at run time and the entry points are resolved by NID, so
// the payload stays self-contained.
#pragma once

#include <cstdint>

#include "core/ports/audio_source.hpp"

namespace bluev
{

class AvcapSource final : public IAudioSource
{
  public:
    ~AvcapSource() override
    {
        close();
    }

    // Loads the library and starts the capture: -1 when the library could
    // not be loaded, 0 when it is not available yet, 1 when it runs.
    int open();
    // Stops the capture. Safe to call again, and when open() failed.
    void close();

    // A record is raw interleaved stereo float PCM; the length in bytes. 0
    // when there is nothing new yet (also after an overrun, which is counted
    // and makes the capture skip to live audio), -1 on error.
    int read(float* buffer, std::uint32_t maxBytes) override;
    bool restart() override;
    long overruns() override
    {
        return m_overruns;
    }

  private:
    bool start();                               // opens and starts one capture
    bool failStep(const char* what, int error); // logs a failed step, remembers it; false
    void teardown();                            // stops, closes and terminates the capture; the library stays loaded

    int (*m_initialize)() = nullptr;
    int (*m_terminate)() = nullptr;
    int (*m_openAudio)(void** handle, void* param, std::uint64_t flags) = nullptr;
    int (*m_close)(void* handle) = nullptr;
    int (*m_start)(void* handle) = nullptr;
    int (*m_stop)(void* handle) = nullptr;
    int (*m_readAudio)(void* handle, void* rec, std::uint32_t max, void* meta, std::uint32_t flag) = nullptr;

    bool m_initialized = false;
    bool m_started = false;
    void* m_handle = nullptr;
    long m_overruns = 0;
    int m_lastError = 0;
};

} // namespace bluev
