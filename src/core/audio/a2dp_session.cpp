// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/a2dp_session.hpp"

#include <cstring>

#include "core/audio/avdtp.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{
constexpr std::uint16_t kAvdtpPsm = 0x19;
}

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

bool A2dpSession::start(bool initiatedByUs)
{
    bool openedByUs = false;
    const auto onFrame = [this](std::uint32_t, const std::uint8_t* buf, int len) { onSignal(buf, len); };
    const auto signalingUp = [this] { return m_sig.up && m_sig.configured; };

    // Many headsets open the AVDTP signaling channel themselves when they
    // reconnect; give them a moment before opening it ourselves. When we made
    // the connection the wait is short: some headsets hang up if the host has
    // not started anything (the Sony WH-CH720N after about eight seconds, the
    // WH-1000XM5 after two to three), while others (the Creative speaker)
    // only open the channel after that. If our own attempt fails the headset
    // still gets the rest of the twenty seconds.
    m_bt.bindInbound(kAvdtpPsm, m_sig, onFrame);
    std::uint64_t deadline = m_clock.nowMs() + (initiatedByUs ? 1000 : 20000);
    while (!m_bt.linkLost() && m_clock.nowMs() < deadline && !signalingUp())
    {
        m_bt.poll(50);
    }
    if (m_sig.connDone && signalingUp())
    {
        logLine("avdtp: the device opened the signaling channel");
    }
    else if (m_bt.openChannel(m_sig, kAvdtpPsm, onFrame))
    {
        openedByUs = true;
        logLine("avdtp: signaling channel opened by us");
    }
    else if (initiatedByUs && !m_bt.linkLost())
    {
        deadline = m_clock.nowMs() + 17000;
        while (!m_bt.linkLost() && m_clock.nowMs() < deadline && !signalingUp())
        {
            m_bt.poll(50);
        }
        if (!(m_sig.connDone && signalingUp()))
        {
            return false;
        }
        logLine("avdtp: the device opened the signaling channel");
    }
    else
    {
        return false;
    }

    // Give the headset time to run the procedure itself. As long as it keeps
    // sending commands it is driving, however slowly (the pause between its
    // commands has been seen to reach two seconds); only a headset that has
    // been silent for four seconds is left to us, and never wait longer than
    // twenty seconds in all.
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
    return m_peerConfigured ? followPeer() : driveSetup();
}

// the device configured the stream itself
bool A2dpSession::followPeer()
{
    const auto onMedia = [](std::uint32_t, const std::uint8_t*, int) {};

    logLine("avdtp: the device configured the stream itself (%02x %02x, bitpool %u-%u)", m_peerCaps & 0xff,
            m_peerCaps >> 8 & 0xff, m_peerCaps >> 16 & 0xff, m_peerCaps >> 24);
    if (!chooseA2dpParams(m_peerCaps, m_config))
    {
        return false;
    }
    std::uint64_t deadline = m_clock.nowMs() + 6000;
    while (!m_bt.linkLost() && m_clock.nowMs() < deadline && !m_peerOpened)
    {
        m_bt.poll(50);
    }
    m_bt.bindInbound(kAvdtpPsm, m_media, onMedia);
    deadline = m_clock.nowMs() + 6000;
    while (!m_bt.linkLost() && m_clock.nowMs() < deadline && !(m_media.up && m_media.configured))
    {
        m_bt.poll(50);
    }
    if (!(m_media.up && m_media.configured))
    {
        logLine("avdtp: the media channel did not open");
        return false;
    }
    deadline = m_clock.nowMs() + 300;
    while (!m_bt.linkLost() && m_clock.nowMs() < deadline && !m_peerStarted)
    {
        m_bt.poll(50);
    }
    if (!m_peerStarted)
    {
        const int st = command(avdtp::kStart, &m_peerSeid, 1);

        if (st != 2)
        {
            logLine("avdtp: start status %d, streaming anyway", st);
        }
        else
        {
            m_peerStarted = true;
        }
    }
    logLine("avdtp: streaming SBC %d Hz, bitpool %d (the device drove it, started=%d)", m_config.rate, m_config.bitpool,
            m_peerStarted);
    return true;
}

// we discover, configure, open and start
bool A2dpSession::driveSetup()
{
    const auto onMedia = [](std::uint32_t, const std::uint8_t*, int) {};
    std::uint8_t sep[1];
    avdtp::Endpoint endpoints[15];

    if (command(avdtp::kDiscover, nullptr, 0) != 2)
    {
        logLine("avdtp: discover failed");
        return false;
    }
    const int count = avdtp::parseEndpoints(m_response, static_cast<int>(m_responseLength), endpoints, 15);

    for (int i = 0; i < count; i++)
    {
        std::uint32_t caps = 0;

        if (!endpoints[i].isSink) // source, not a sink
        {
            continue;
        }
        sep[0] = static_cast<std::uint8_t>(endpoints[i].seid << 2);
        if (command(avdtp::kGetCapabilities, sep, 1) != 2)
        {
            continue;
        }
        if (static_cast<int>(m_responseLength) < 4)
        {
            continue;
        }
        if (!avdtp::findSbcCapabilities(m_response, static_cast<int>(m_responseLength), caps))
        {
            continue;
        }

        m_sinkSeid = endpoints[i].seid;
        logLine("avdtp: SBC sink %d, capabilities %02x %02x, bitpool %u-%u", m_sinkSeid, caps & 0xff, caps >> 8 & 0xff,
                caps >> 16 & 0xff, caps >> 24);

        if (!chooseA2dpParams(caps, m_config))
        {
            return false;
        }

        // SET_CONFIGURATION with the SBC media codec capabilities
        std::uint8_t cfg[12] = {};
        cfg[0] = static_cast<std::uint8_t>(m_sinkSeid << 2);
        cfg[1] = 0x04;
        cfg[2] = 0x01;
        cfg[3] = 0x00;
        cfg[4] = 0x07;
        cfg[5] = 0x06;
        cfg[6] = 0x00;
        cfg[7] = 0x00;
        cfg[8] = m_config.freqMode;
        cfg[9] = m_config.cfg;
        cfg[10] = static_cast<std::uint8_t>(m_config.bitpool);
        cfg[11] = static_cast<std::uint8_t>(m_config.bitpool);
        if (command(avdtp::kSetConfiguration, cfg, 12) != 2)
        {
            logLine("avdtp: configuration refused (rsp %02x %02x %02x %02x, sent %02x %02x)", m_response[0],
                    m_response[1], m_response[2], m_response[3], cfg[8], cfg[9]);
            return false;
        }
        sep[0] = static_cast<std::uint8_t>(m_sinkSeid << 2);
        if (command(avdtp::kOpen, sep, 1) != 2)
        {
            logLine("avdtp: open refused");
            return false;
        }

        // the headset opens the media channel towards us
        m_bt.bindInbound(kAvdtpPsm, m_media, onMedia);
        std::uint64_t deadline = m_clock.nowMs() + 1500;
        if (m_clock.nowMs() < deadline && !m_media.up)
        {
            while (!m_bt.linkLost())
            {
                m_bt.poll(50);
                if (m_clock.nowMs() >= deadline || m_media.up)
                {
                    break;
                }
            }
        }

        if (!(m_media.connDone && m_media.up && m_media.configured))
        {
            if (!m_bt.openChannel(m_media, kAvdtpPsm, onMedia))
            {
                return false;
            }
        }
        deadline = m_clock.nowMs() + 300;
        while (!m_bt.linkLost() && m_clock.nowMs() < deadline)
        {
            m_bt.poll(50);
        }

        sep[0] = static_cast<std::uint8_t>(m_sinkSeid << 2);
        const int st = command(avdtp::kStart, sep, 1);
        if (st != 2)
        {
            logLine("avdtp: start status %d, streaming anyway", st);
        }
        logLine("avdtp: streaming SBC %d Hz, bitpool %d", m_config.rate, m_config.bitpool);
        return true;
    }

    logLine("avdtp: the device has no SBC sink");
    return false;
}

// the device's commands, and the answers to ours
void A2dpSession::onSignal(const std::uint8_t* buf, int len)
{
    avdtp::Header h;
    std::uint8_t rsp[16];

    if (!avdtp::parseHeader(buf, len, h))
    {
        return;
    }
    if (h.type == avdtp::kCommand)
    {
        m_peerLastCommand = m_clock.nowMs(); // the headset is driving
    }

    if (h.type == avdtp::kCommand && h.signal == avdtp::kDiscover)
    {
        // some sinks ask us first and close the channel when we refuse;
        // offer one SBC source endpoint
        m_bt.send(m_sig.theirCid, rsp, static_cast<int>(avdtp::buildDiscoverAccept(h.label, rsp)));
        return;
    }
    if (h.type == avdtp::kCommand && h.signal == avdtp::kGetCapabilities)
    {
        m_bt.send(m_sig.theirCid, rsp, static_cast<int>(avdtp::buildCapabilitiesAccept(h.label, rsp)));
        return;
    }
    avdtp::PeerConfig peer;
    if (h.type == avdtp::kCommand && h.signal == avdtp::kSetConfiguration &&
        avdtp::parseSetConfiguration(buf, len, peer))
    {
        // take the headset's choice
        m_peerSeid = peer.seid;
        if (peer.haveSbc)
        {
            m_peerCaps = peer.caps;
            m_peerConfigured = true;
        }
        m_bt.send(m_sig.theirCid, rsp, static_cast<int>(avdtp::buildAccept(h.label, avdtp::kSetConfiguration, rsp)));
        return;
    }
    if (h.type == avdtp::kCommand && (h.signal == avdtp::kOpen || h.signal == avdtp::kStart))
    {
        m_bt.send(m_sig.theirCid, rsp, static_cast<int>(avdtp::buildAccept(h.label, h.signal, rsp)));
        if (h.signal == avdtp::kOpen)
        {
            m_peerOpened = true;
        }
        else
        {
            m_peerStarted = true;
        }
        return;
    }
    if (h.type == avdtp::kCommand)
    { // anything else: general reject
        m_bt.send(m_sig.theirCid, rsp, static_cast<int>(avdtp::buildReject(h.label, h.signal, 0x19, rsp)));
        return;
    }
    if (h.label == m_label)
    { // our pending transaction
        m_responseLength = len < 256 ? static_cast<std::uint32_t>(len) : 256;
        std::memcpy(m_response, buf, m_responseLength);
        m_responseReady = true;
    }
}

} // namespace bluev
