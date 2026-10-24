// SPDX-License-Identifier: GPL-3.0-or-later
// Who is told how the stream is doing (the service, which shows it on the page).
#pragma once

namespace bluev
{

// Live numbers from the streaming loop, a few times a second.
class IStreamObserver
{
  public:
    virtual ~IStreamObserver() = default;
    // `rate` is the sample rate in Hz (0 when not streaming), `queueMs` the audio
    // waiting to be sent, `packets` the total since the stream started. Called
    // from the streaming thread, so an implementation must not block.
    virtual void onStream(int rate, int queueMs, int packetsPerSecond, long packets) = 0;
};

} // namespace bluev
