// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/pcm_fifo.hpp"

#include <algorithm>
#include <cstring>

namespace bluev
{

namespace
{
std::int16_t toSample(float value)
{
    return static_cast<std::int16_t>(static_cast<std::int32_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}
} // namespace

void PcmFifo::push(float l, float r)
{
    if (m_size >= kCapacityFrames)
    {
        return;
    }
    m_samples[static_cast<std::size_t>(m_size) * 2] = toSample(l);
    m_samples[static_cast<std::size_t>(m_size) * 2 + 1] = toSample(r);
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
