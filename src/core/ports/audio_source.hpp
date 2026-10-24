// SPDX-License-Identifier: GPL-3.0-or-later
// The audio the console is playing, as 48 kHz stereo float samples.
#pragma once

#include <cstdint>

namespace bluev
{

class IAudioSource
{
  public:
    virtual ~IAudioSource() = default;

    // Reads one record into `buffer` (up to `maxBytes`, stereo float).
    // The length in bytes (at least 8 for a usable record), 0 when there is
    // nothing new, and a negative number after an error.
    virtual int read(float* buffer, std::uint32_t maxBytes) = 0;
    // Start capturing again after an error; true when it works.
    virtual bool restart() = 0;
    // How many times the capture ran ahead of us and skipped to live audio.
    virtual long overruns() = 0;
};

} // namespace bluev
