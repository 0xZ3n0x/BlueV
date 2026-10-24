// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/resampler.hpp"

namespace bluev
{

void LinearResampler::process(const float* rec, int frames, PcmFifo& out)
{
    const double step = 48000.0 / m_rate;

    for (int f = 0; f < frames; f++)
    {
        const float l = rec[f * 2], r = rec[f * 2 + 1];

        while (m_pos < 1.0)
        {
            out.push(m_prev[0] + static_cast<float>(m_pos) * (l - m_prev[0]),
                     m_prev[1] + static_cast<float>(m_pos) * (r - m_prev[1]));
            m_pos += step;
        }
        m_prev[0] = l;
        m_prev[1] = r;
        m_pos -= 1.0;
    }
}

} // namespace bluev
