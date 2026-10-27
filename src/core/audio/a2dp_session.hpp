// SPDX-License-Identifier: GPL-3.0-or-later
// The AVDTP/A2DP session: finds the device's SBC sink, configures it and
// opens the media channel, or follows the device when it drives the
// procedure itself.
#pragma once

#include <atomic>
#include <cstdint>

#include "core/audio/a2dp_params.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "core/ports/clock.hpp"

namespace bluev
{

class A2dpSession
{
  public:
    A2dpSession(BtStack& bt, IClock& clock);

    // Accept the signaling channel when the device opens it, even before a
    // session has started (it dials in as soon as the link is up).
    void listen();
    // Forget everything about the previous session, including a stop request.
    void reset();

    // `initiatedByUs`: we made the connection, so the device is not expected
    // to start the AVDTP procedure for long.
    // Waits for the device to open the signaling channel (or opens it), then
    // either follows the device's own setup or drives it: discover, configure,
    // open. False when the link goes or no media channel results.
    bool start(bool initiatedByUs);
    // Closes the media and signaling channels (does nothing once the link is lost).
    void stop();

    // stop request from the service, read by the streaming loop
    void requestStop()
    {
        m_stopRequested.store(true);
    }
    bool stopRequested() const
    {
        return m_stopRequested.load();
    }

    // The result of start(): what is being streamed, and the two channels.
    const A2dpConfig& config() const
    {
        return m_config;
    }
    L2Channel& media()
    {
        return m_media;
    }
    L2Channel& signaling()
    {
        return m_sig;
    }

  private:
    void onSignal(const std::uint8_t* buf, int len);
    int command(std::uint8_t signal, const std::uint8_t* params, int len); // the answer's type (2 = accept), or -1
    bool followPeer();                                                     // the device configured the stream itself
    bool driveSetup();                                                     // we configure it
    bool findSbcSink(std::uint32_t& caps);
    bool sendStart(std::uint8_t seidByte);

    BtStack& m_bt;
    IClock& m_clock;
    L2Channel m_sig{"signaling", 0x40};
    L2Channel m_media{"media", 0x42};
    A2dpConfig m_config;

    std::uint8_t m_sinkSeid = 0;       // the device's SBC sink endpoint
    std::uint8_t m_label = 0;          // transaction label of the next command
    std::uint8_t m_response[256] = {}; // the answer to the command in flight
    std::uint32_t m_responseLength = 0;
    bool m_responseReady = false;

    // the device may drive the AVDTP procedure itself when it reconnects
    bool m_peerConfigured = false, m_peerOpened = false, m_peerStarted = false;
    std::uint32_t m_peerCaps = 0;        // the SBC setting it chose
    std::uint8_t m_peerSeid = 0;         // the endpoint it configured, as the byte it sent (seid << 2)
    std::uint64_t m_peerLastCommand = 0; // when its last command arrived, in clock ms

    std::atomic<bool> m_stopRequested{false};
};

} // namespace bluev
