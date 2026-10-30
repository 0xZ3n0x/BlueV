// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio mix (what Remote Play uses) as the program's audio
// source: starts the capture in the library's order, takes it down again when
// a step fails, and turns what ReadAudio answers into "a record", "nothing new
// yet", "ran ahead" or "error".
#pragma once

#include <cstdint>

#include "core/ports/audio_source.hpp"
#include "core/ports/avcap.hpp"

namespace bluev
{

class AvcapCapture final : public IAudioSource
{
  public:
    explicit AvcapCapture(IAvcap2& library) : m_library(library)
    {
    }
    ~AvcapCapture() override
    {
        close();
    }
    AvcapCapture(const AvcapCapture&) = delete;
    AvcapCapture& operator=(const AvcapCapture&) = delete;

    // Opens and starts one capture. False when the console does not give it
    // yet (at start-up the audio service may not be ready).
    bool start();
    // Stops the capture. Safe to call again, and after start() failed.
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
    bool failStep(const char* what, int error); // logs a failed step, remembers it; false
    void teardown();                            // stops, closes and terminates the capture

    IAvcap2& m_library;
    bool m_initialized = false;
    bool m_started = false;
    void* m_handle = nullptr;
    long m_overruns = 0;
    int m_lastError = 0;
};

} // namespace bluev
