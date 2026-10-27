// SPDX-License-Identifier: GPL-3.0-or-later
// The console's audio as the streaming loop sees it: a sine tone that grows
// with the clock, 48 kHz stereo float.
#pragma once

#include <cmath>
#include <functional>

#include "core/ports/audio_source.hpp"
#include "core/audio/a2dp_streamer.hpp"
#include "fake_clock.hpp"

namespace bluev::testing
{

class FakeAudio final : public IAudioSource
{
  public:
    explicit FakeAudio(FakeClock& clock) : m_clock(clock), m_last(clock.nowMs())
    {
    }

    int read(float* buffer, std::uint32_t maxBytes) override
    {
        reads++;
        if (onRead)
        {
            onRead();
        }
        if (failNext)
        {
            failNext = false;
            return -1;
        }
        if (silent)
        {
            return 0;
        }
        const std::uint64_t now = m_clock.nowMs();
        if (now - m_last < minIntervalMs)
        {
            return 0;
        }
        std::uint64_t frames = (now - m_last) * 48;

        if (frames < 1)
        {
            return 0;
        }
        if (frames > maxBytes / 8)
        {
            frames = maxBytes / 8;
        }
        m_last += frames / 48;
        for (std::uint64_t i = 0; i < frames; i++)
        {
            const float v = amplitude * std::sin(m_phase);
            m_phase += 2.0f * 3.14159265f * 440.0f / 48000.0f;
            buffer[i * 2] = v;
            buffer[i * 2 + 1] = v;
        }
        framesDelivered += static_cast<long>(frames);
        return static_cast<int>(frames * 8);
    }
    bool restart() override
    {
        restarts++;
        if (!restartWorks)
        {
            return false;
        }
        m_last = m_clock.nowMs();
        silent = false;
        return true;
    }
    long overruns() override
    {
        return 7;
    }

    bool silent = false;   // read() returns nothing
    bool failNext = false; // the next read() is an error
    bool restartWorks = true;
    float amplitude = 0.5f;
    std::uint32_t minIntervalMs = 0; // audio comes in bursts of this many milliseconds
    int reads = 0, restarts = 0;
    long framesDelivered = 0;
    std::function<void()> onRead;

  private:
    FakeClock& m_clock;
    std::uint64_t m_last;
    float m_phase = 0.0f;
};

class RecordingObserver
{
  public:
    StreamObserver hook()
    {
        return [this](int r, int q, int p, long n) {
            count++;
            rate = r;
            queueMs = q;
            pps = p;
            packets = n;
        };
    }
    int count = 0, rate = 0, queueMs = 0, pps = 0;
    long packets = 0;
};

} // namespace bluev::testing
