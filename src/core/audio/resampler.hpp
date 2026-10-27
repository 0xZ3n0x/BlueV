// SPDX-License-Identifier: GPL-3.0-or-later
// Linear 48 kHz -> rate conversion for devices that take only 44.1 kHz
// (the capture is always 48 kHz).
#pragma once

#include <cstdint>

#include "core/audio/pcm_fifo.hpp"

namespace bluev
{

class LinearResampler
{
  public:
    explicit LinearResampler(std::uint32_t targetRate = 44100) : m_rate(targetRate)
    {
    }

    // forget the position and the last frame (start of a stream)
    // `frames` stereo frames of 48 kHz float, interleaved; the converted
    // frames are pushed to `out` as 16 bit.
    void process(const float* interleaved, int frames, PcmFifo& out);

  private:
    std::uint32_t m_rate;
    double m_pos = 0.0;
    float m_prev[2] = {0.0f, 0.0f};
};

} // namespace bluev
