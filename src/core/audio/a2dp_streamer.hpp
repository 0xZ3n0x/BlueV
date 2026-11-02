// SPDX-License-Identifier: GPL-3.0-or-later
// The streaming loop: reads the console's audio, resamples when the device
// needs 44.1 kHz, encodes SBC and sends RTP media packets, paced to the clock.
#pragma once

#include "core/audio/a2dp_session.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "core/ports/audio_source.hpp"
#include "core/ports/clock.hpp"
#include <functional>

namespace bluev
{

// Gets the stream's live numbers a few times a second (rate, queue in ms, packets per second, packets sent).
using StreamObserver = std::function<void(int rate, int queueMs, int packetsPerSecond, long packets)>;

class A2dpStreamer
{
  public:
    // `observer` may be empty
    A2dpStreamer(BtStack& bt, A2dpSession& session, IAudioSource& audio, IClock& clock, StreamObserver observer);

    // Runs until the link is lost, the media channel closes or a stop is
    // requested. `capturing`: the audio source is already running; otherwise
    // the stream starts with silence and tries to start the capture. True
    // when at least one packet went out.
    bool run(bool capturing);

    void setObserver(StreamObserver observer)
    {
        m_observer = std::move(observer);
    }

  private:
    BtStack& m_bt;
    A2dpSession& m_session;
    IAudioSource& m_audio;
    IClock& m_clock;
    StreamObserver m_observer;
};

} // namespace bluev
