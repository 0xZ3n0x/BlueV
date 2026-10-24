// SPDX-License-Identifier: GPL-3.0-or-later
// The streaming loop: reads the console's audio, resamples when the device
// needs 44.1 kHz, encodes SBC and sends RTP media packets, paced to the clock.
#pragma once

#include "core/audio/a2dp_session.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "core/ports/audio_source.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/stream_observer.hpp"

namespace bluev
{

class A2dpStreamer
{
  public:
    // `observer` may be null
    A2dpStreamer(BtStack& bt, A2dpSession& session, IAudioSource& audio, IClock& clock, IStreamObserver* observer);

    // Runs until the link is lost, the media channel closes or a stop is
    // requested. `capturing`: the audio source is already running; otherwise
    // the stream starts with silence and tries to start the capture. True
    // when at least one packet went out.
    bool run(bool capturing);

    // the numbers go here a few times a second (null = nobody)
    void setObserver(IStreamObserver* observer)
    {
        m_observer = observer;
    }

  private:
    BtStack& m_bt;
    A2dpSession& m_session;
    IAudioSource& m_audio;
    IClock& m_clock;
    IStreamObserver* m_observer;
};

} // namespace bluev
