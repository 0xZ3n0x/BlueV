// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/a2dp_session.hpp"

#include <cstring>

#include "core/audio/avdtp.hpp"
#include "core/hex.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{
constexpr std::uint16_t kAvdtpPsm = 0x19;

// the media channel carries no frames we need
void ignoreMedia(std::uint32_t, const std::uint8_t*, int)
{
}
} // namespace

A2dpSession::A2dpSession(BtStack& bt, IClock& clock) : m_bt(bt), m_clock(clock)
{
}

void A2dpSession::listen()
{
    m_bt.bindInbound(kAvdtpPsm, m_sig, [this](std::uint32_t, const std::uint8_t* buf, int len) { onSignal(buf, len); });
}

void A2dpSession::reset()
{
    m_sig.reset();
    m_media.reset();
    m_peerConfigured = false;
    m_peerOpened = false;
    m_peerStarted = false;
    m_peerCaps = 0;
    m_peerSeid = 0;
    m_peerLastCommand = 0;
    m_responseReady = false;
    m_stopRequested.store(false);
}

// send one AVDTP signaling command and wait for its answer; the type of the
// answer (2 = accept) or -1
int A2dpSession::command(std::uint8_t signal, const std::uint8_t* params, int len)
{
    std::uint8_t cmd[2 + 62];

    for (int attempt = 0; attempt < 2; attempt++)
    {
        m_label = (m_label + 1) & 0xf;
        const std::size_t n = avdtp::buildCommand(m_label, signal, params, len, cmd);
        m_responseReady = false;
        logLine("avdtp: > command %#04x label %d: %s", signal, m_label, hexStart(cmd, static_cast<int>(n)).c_str());
        if (!m_bt.send(m_sig.theirCid, cmd, static_cast<int>(n)))
        {
            return -1;
        }
        if (m_bt.wait([this] { return m_responseReady; }, 8000))
        {
            return m_response[0] & 3;
        }
        if (m_bt.linkLost())
        {
            break;
        }
    }
    logLine("avdtp: signal %#04x unanswered", signal);
    return -1;
}

void A2dpSession::stop()
{
    const std::uint8_t sep = static_cast<std::uint8_t>(m_sinkSeid << 2);

    if (m_bt.linkLost())
    {
        return;
    }
    if (m_media.theirCid != 0 && !m_media.closed)
    {
        command(avdtp::kClose, &sep, 1);
        m_bt.closeChannel(m_media);
    }
    m_bt.closeChannel(m_sig);
}

// Gets the AVDTP signaling channel up, from either side. `openedByUs` tells
// which side did it. False when neither did.
bool A2dpSession::openSignaling(bool initiatedByUs, bool& openedByUs)
{
    const auto onFrame = [this](std::uint32_t, const std::uint8_t* buf, int len) { onSignal(buf, len); };
    const auto signalingUp = [this] { return m_sig.up && m_sig.configured; };

    openedByUs = false;

    // Many headsets open the AVDTP signaling channel themselves when they
    // reconnect; give them a moment before opening it ourselves. When we made
    // the connection the wait is short: some headsets hang up if the host has
    // not started anything (the Sony WH-CH720N after about eight seconds, the
    // WH-1000XM5 after two to three), while others (the Creative speaker)
    // only open the channel after that. If our own attempt fails the headset
    // still gets the rest of the twenty seconds.
    m_bt.bindInbound(kAvdtpPsm, m_sig, onFrame);
    m_bt.wait(signalingUp, initiatedByUs ? 1000 : 20000);
    if (m_sig.connDone && signalingUp())
    {
        logLine("avdtp: the device opened the signaling channel");
        return true;
    }
    if (m_bt.openChannel(m_sig, kAvdtpPsm, onFrame))
    {
        openedByUs = true;
        logLine("avdtp: signaling channel opened by us");
        return true;
    }
    if (!initiatedByUs || m_bt.linkLost())
    {
        return false;
    }
    m_bt.wait(signalingUp, 17000);
    if (!(m_sig.connDone && signalingUp()))
    {
        return false;
    }
    logLine("avdtp: the device opened the signaling channel");
    return true;
}

// Give the headset time to run the procedure itself. As long as it keeps
// sending commands it is driving, however slowly (the pause between its
// commands has been seen to reach two seconds); only a headset that has been
// silent for four seconds is left to us, and never wait longer than twenty
// seconds in all.
void A2dpSession::waitForPeerSetup(bool openedByUs)
{
    const std::uint64_t began = m_clock.nowMs();

    while (!m_bt.linkLost() && !m_peerConfigured)
    {
        const std::uint64_t now = m_clock.nowMs();
        const std::uint64_t quietSince = m_peerLastCommand > began ? m_peerLastCommand : began;

        if (now - quietSince >= (openedByUs ? 1500u : 4000u) || now - began >= 20000)
        {
            break;
        }
        m_bt.poll(50);
    }
}

bool A2dpSession::start(bool initiatedByUs)
{
    bool openedByUs = false;

    if (!openSignaling(initiatedByUs, openedByUs))
    {
        return false;
    }
    waitForPeerSetup(openedByUs);
    return m_peerConfigured ? followPeer() : driveSetup();
}

// the device configured the stream itself
bool A2dpSession::followPeer()
{
    logLine("avdtp: the device configured the stream itself (%02x %02x, bitpool %u-%u)", m_peerCaps & 0xff,
            m_peerCaps >> 8 & 0xff, m_peerCaps >> 16 & 0xff, m_peerCaps >> 24);
    if (!chooseA2dpParams(m_peerCaps, m_config))
    {
        return false;
    }

    // it opens the media channel towards us; wait for that, and for its Start
    m_bt.wait([this] { return m_peerOpened; }, 6000);
    m_bt.bindInbound(kAvdtpPsm, m_media, ignoreMedia);
    m_bt.wait([this] { return m_media.up && m_media.configured; }, 6000);
    if (!(m_media.up && m_media.configured))
    {
        logLine("avdtp: the media channel did not open");
        return false;
    }
    m_bt.wait([this] { return m_peerStarted; }, 300);
    if (!m_peerStarted)
    {
        m_peerStarted = sendStart(m_peerSeid);
    }
    logLine("avdtp: streaming SBC %d Hz, bitpool %d (the device drove it, started=%d)", m_config.rate, m_config.bitpool,
            m_peerStarted);
    return true;
}

// Sends Start for the endpoint `seidByte` (seid << 2). A device that refuses is
// streamed to anyway.
bool A2dpSession::sendStart(std::uint8_t seidByte)
{
    const int status = command(avdtp::kStart, &seidByte, 1);

    if (status != 2)
    {
        logLine("avdtp: start status %d, streaming anyway", status);
    }
    return status == 2;
}

// Asks the device what it has and picks its first SBC sink (m_sinkSeid, and the
// SBC capabilities in `caps`).
bool A2dpSession::findSbcSink(std::uint32_t& caps)
{
    avdtp::Endpoint endpoints[15];

    if (command(avdtp::kDiscover, nullptr, 0) != 2)
    {
        logLine("avdtp: discover failed");
        return false;
    }
    const int count = avdtp::parseEndpoints(m_response, static_cast<int>(m_responseLength), endpoints, 15);

    for (int i = 0; i < count; i++)
    {
        const std::uint8_t seidByte = static_cast<std::uint8_t>(endpoints[i].seid << 2);

        if (!endpoints[i].isSink) // source, not a sink
        {
            continue;
        }
        if (command(avdtp::kGetCapabilities, &seidByte, 1) != 2 || static_cast<int>(m_responseLength) < 4 ||
            !avdtp::findSbcCapabilities(m_response, static_cast<int>(m_responseLength), caps))
        {
            continue;
        }
        m_sinkSeid = endpoints[i].seid;
        return true;
    }
    logLine("avdtp: the device has no SBC sink");
    return false;
}

// we configure the stream: pick the sink, set the codec, open, start
bool A2dpSession::driveSetup()
{
    std::uint32_t caps = 0;

    if (!findSbcSink(caps))
    {
        return false;
    }
    const std::uint8_t sink = static_cast<std::uint8_t>(m_sinkSeid << 2);

    logLine("avdtp: SBC sink %d, capabilities %02x %02x, bitpool %u-%u", m_sinkSeid, caps & 0xff, caps >> 8 & 0xff,
            caps >> 16 & 0xff, caps >> 24);
    if (!chooseA2dpParams(caps, m_config))
    {
        return false;
    }

    // Set Configuration: media transport, then the SBC codec with our choice
    const std::uint8_t cfg[12] = {sink,
                                  0x04,
                                  0x01,
                                  0x00,
                                  0x07,
                                  0x06,
                                  0x00,
                                  0x00,
                                  m_config.freqMode,
                                  m_config.cfg,
                                  static_cast<std::uint8_t>(m_config.bitpool),
                                  static_cast<std::uint8_t>(m_config.bitpool)};

    if (command(avdtp::kSetConfiguration, cfg, 12) != 2)
    {
        logLine("avdtp: configuration refused (rsp %02x %02x %02x %02x, sent %02x %02x)", m_response[0], m_response[1],
                m_response[2], m_response[3], cfg[8], cfg[9]);
        return false;
    }
    if (command(avdtp::kOpen, &sink, 1) != 2)
    {
        logLine("avdtp: open refused");
        return false;
    }

    // the headset opens the media channel towards us; open it ourselves when it does not
    m_bt.bindInbound(kAvdtpPsm, m_media, ignoreMedia);
    m_bt.wait([this] { return m_media.up; }, 1500);
    if (!(m_media.connDone && m_media.up && m_media.configured) && !m_bt.openChannel(m_media, kAvdtpPsm, ignoreMedia))
    {
        return false;
    }
    m_bt.wait([] { return false; }, 300); // let the channel settle

    sendStart(sink);
    logLine("avdtp: streaming SBC %d Hz, bitpool %d", m_config.rate, m_config.bitpool);
    return true;
}

void A2dpSession::onSignal(const std::uint8_t* buf, int len)
{
    avdtp::Header h;
    avdtp::PeerConfig peer;
    std::uint8_t rsp[16];
    std::size_t n;

    if (!avdtp::parseHeader(buf, len, h))
    {
        logLine("avdtp: < unreadable, %d bytes: %s", len, hexStart(buf, len).c_str());
        return;
    }
    logLine("avdtp: < %s %#04x label %d: %s", h.type == avdtp::kCommand ? "command" : "answer", h.signal, h.label,
            hexStart(buf, len).c_str());
    if (h.type != avdtp::kCommand)
    {
        if (h.label == m_label)
        { // the answer to our pending transaction
            m_responseLength = len < 256 ? static_cast<std::uint32_t>(len) : 256;
            std::memcpy(m_response, buf, m_responseLength);
            m_responseReady = true;
        }
        return;
    }

    m_peerLastCommand = m_clock.nowMs(); // the headset is driving
    if (h.signal == avdtp::kDiscover)
    {
        n = avdtp::buildDiscoverAccept(h.label, rsp);
    }
    else if (h.signal == avdtp::kGetCapabilities || h.signal == avdtp::kGetAllCapabilities)
    {
        n = avdtp::buildCapabilitiesAccept(h.label, h.signal, rsp);
    }
    else if (h.signal == avdtp::kSetConfiguration && avdtp::parseSetConfiguration(buf, len, peer))
    {
        m_peerSeid = peer.seid;
        if (peer.haveSbc)
        {
            m_peerCaps = peer.caps;
            m_peerConfigured = true;
        }
        n = avdtp::buildAccept(h.label, avdtp::kSetConfiguration, rsp);
    }
    else if (h.signal == avdtp::kOpen || h.signal == avdtp::kStart)
    {
        (h.signal == avdtp::kOpen ? m_peerOpened : m_peerStarted) = true;
        n = avdtp::buildAccept(h.label, h.signal, rsp);
    }
    else
    { // anything else: general reject
        n = avdtp::buildReject(h.label, h.signal, 0x19, rsp);
    }
    logLine("avdtp: > %s %#04x label %d", (rsp[0] & 3) == avdtp::kAccept ? "accept" : "reject", h.signal, h.label);
    m_bt.send(m_sig.theirCid, rsp, static_cast<int>(n));
}

} // namespace bluev
