// SPDX-License-Identifier: GPL-3.0-or-later
// The queue between the capture and the encoder: 16 bit stereo frames.
#pragma once

#include <array>
#include <cstdint>

namespace bluev
{

class PcmFifo
{
  public:
    static constexpr int kCapacityFrames = 24000;

    // frames queued
    int size() const
    {
        return m_size;
    }
    void clear()
    {
        m_size = 0;
    }

    // Clamps to -1..1 and stores as 16 bit; drops the frame when full.
    void push(float left, float right);
    // Zero frames (silence), as far as there is room.
    void pushSilence(int frames);
    // Removes the oldest frames.
    void dropFront(int frames);

    // the oldest frames, interleaved left/right
    const std::int16_t* data() const
    {
        return m_samples.data();
    }

  private:
    std::array<std::int16_t, kCapacityFrames * 2> m_samples{};
    int m_size = 0;
};

} // namespace bluev
