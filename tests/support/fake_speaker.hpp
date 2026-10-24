// SPDX-License-Identifier: GPL-3.0-or-later
// The Bluetooth speaker on the other end of a FakeController: answers the
// L2CAP signaling, speaks AVDTP, and keeps the media packets it receives.
// It can also be the one that drives the procedure.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fake_controller.hpp"

namespace bluev::testing
{

struct MediaPacket
{
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    int sbcFrames = 0;
    int payloadBytes = 0; // after the 13 byte header
    std::uint64_t at = 0; // when it arrived
};

class FakeSpeaker
{
  public:
    FakeSpeaker(FakeController& controller, FakeClock& clock);

    // takes over the controller's ACL output
    void attach();

    // ---- how it behaves ----
    bool hasSbcSink = true; // otherwise it lists a source only
    // the SBC element it offers: byte 1 | byte 2 << 8 | min pool << 16 | max pool << 24
    std::uint32_t sbcCaps = 0x3502ffff;
    bool refuseConfiguration = false;
    bool opensMediaItself = true;   // after Open, it connects the media channel
    bool driveSendsStart = true;    // when it drives, it also sends Start
    bool silentAvdtp = false;       // no answer to AVDTP commands
    bool reportsCompletions = true; // "number of completed packets" for every packet
    std::uint16_t sigCid = 0x0140, mediaCid = 0x0141;
    std::uint16_t mtu = 672;

    // The speaker drives the procedure from `delayMs` on: it connects the
    // signaling channel, sets the configuration, opens, connects media, starts.
    void drive(std::uint32_t delayMs, std::uint32_t caps);
    // It connects the signaling channel by itself, and waits for us to run AVDTP.
    void dialIn(std::uint32_t delayMs);
    // an AVDTP command from the speaker on the signaling channel
    void sendCommand(std::uint8_t label, std::uint8_t signal, const Bytes& params, std::uint32_t delayMs = 0);
    // the speaker closes the media channel
    void closeMedia(std::uint32_t delayMs = 0);

    // ---- what it saw ----
    std::vector<std::uint8_t> signals; // AVDTP commands received, in order
    Bytes configuration;               // the Set Configuration parameters
    std::vector<MediaPacket> media;
    bool started = false;
    bool closed = false;
    int rejectedGeneral = 0; // general rejects it received
    Bytes lastAvdtpReply;    // the last AVDTP answer from the stack to a command of ours

  private:
    void onAcl(const Bytes& packet);
    void onSignaling(const Bytes& payload);
    void onAvdtp(const Bytes& payload);
    void l2capSig(const Bytes& cmd, std::uint32_t delayMs = 0);
    void avdtp(const Bytes& payload, std::uint32_t delayMs = 0);
    void connectRequest(std::uint16_t scid, std::uint32_t delayMs);
    void accept(std::uint8_t label, std::uint8_t signal, const Bytes& params = {});

    FakeController& m_controller;
    FakeClock& m_clock;
    int m_channelsAccepted = 0;
    std::uint16_t m_ourSigCid = 0, m_ourMediaCid = 0; // the stack's cids for the channels it accepted from us
    bool m_driving = false;
    std::uint32_t m_driveCaps = 0;
    std::uint8_t m_label = 0;
};

} // namespace bluev::testing
