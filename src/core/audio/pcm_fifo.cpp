// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/pcm_fifo.hpp"

#include <cstring>

namespace bluev
{

void PcmFifo::push(float l, float r)
{
    if (m_size >= kCapacityFrames)
    {
        return;
    }
    if (l > 1.0f)
    {
        l = 1.0f;
    }
    if (l < -1.0f)
    {
        l = -1.0f;
    }
    if (r > 1.0f)
    {
        r = 1.0f;
    }
    if (r < -1.0f)
    {
        r = -1.0f;
    }
    m_samples[static_cast<std::size_t>(m_size) * 2] =
        static_cast<std::int16_t>(static_cast<std::int32_t>(l * 32767.0f));
    m_samples[static_cast<std::size_t>(m_size) * 2 + 1] =
        static_cast<std::int16_t>(static_cast<std::int32_t>(r * 32767.0f));
    m_size++;
}

void PcmFifo::pushSilence(int frames)
{
    while (frames-- > 0)
    {
        if (m_size < kCapacityFrames)
        {
            m_samples[static_cast<std::size_t>(m_size) * 2] = 0;
            m_samples[static_cast<std::size_t>(m_size) * 2 + 1] = 0;
            m_size++;
        }
    }
}

void PcmFifo::dropFront(int frames)
{
    if (frames <= 0)
    {
        return;
    }
    if (frames > m_size)
    {
        frames = m_size;
    }
    m_size -= frames;
    std::memmove(m_samples.data(), m_samples.data() + static_cast<std::size_t>(frames) * 2,
                 static_cast<std::size_t>(m_size) * 4);
}

} // namespace bluev
