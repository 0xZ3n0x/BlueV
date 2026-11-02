// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_speaker.hpp"

#include <algorithm>

namespace bluev::testing
{

namespace
{
Bytes u16(std::uint16_t v)
{
    return {static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>(v >> 8)};
}
void append(Bytes& to, const Bytes& from)
{
    to.insert(to.end(), from.begin(), from.end());
}
} // namespace

FakeSpeaker::FakeSpeaker(FakeController& controller, FakeClock& clock) : m_controller(controller), m_clock(clock)
{
}

void FakeSpeaker::attach()
{
    m_controller.onAclOut = [this](const Bytes& packet) { onAcl(packet); };
}

void FakeSpeaker::l2capSig(const Bytes& cmd, std::uint32_t delayMs)
{
    m_controller.l2cap(1, cmd, 0, delayMs);
}

void FakeSpeaker::avdtp(const Bytes& payload, std::uint32_t delayMs)
{
    m_controller.l2cap(m_ourSigCid ? m_ourSigCid : 0x40, payload, 0, delayMs);
}

void FakeSpeaker::connectRequest(std::uint16_t scid, std::uint32_t delayMs)
{
    Bytes c = {0x02, 0x71, 4, 0, 0x19, 0x00};
    append(c, u16(scid));
    l2capSig(c, delayMs);
}

void FakeSpeaker::accept(std::uint8_t label, std::uint8_t signal, const Bytes& params)
{
    Bytes r = {static_cast<std::uint8_t>(label << 4 | 2), signal};
    append(r, params);
    if (signal == loseAnswerTo)
    {
        loseAnswerTo = 0;
        return;
    }
    if (!silentAvdtp)
    {
        avdtp(r);
    }
}

void FakeSpeaker::sendCommand(std::uint8_t label, std::uint8_t signal, const Bytes& params, std::uint32_t delayMs)
{
    Bytes r = {static_cast<std::uint8_t>(label << 4), signal};
    append(r, params);
    avdtp(r, delayMs);
}

void FakeSpeaker::closeMedia(std::uint32_t delayMs)
{
    Bytes c = {0x06, 0x72, 4, 0};
    append(c, u16(m_ourMediaCid));
    append(c, u16(mediaCid));
    l2capSig(c, delayMs);
}

void FakeSpeaker::dialIn(std::uint32_t delayMs)
{
    connectRequest(sigCid, delayMs);
}

void FakeSpeaker::drive(std::uint32_t delayMs, std::uint32_t caps)
{
    m_driving = true;
    m_driveCaps = caps;
    // signaling channel first; the rest follows when its configuration is done
    connectRequest(sigCid, delayMs);
    // the speaker's own AVDTP procedure, with the pauses of a slow device:
    // our endpoint (seid 1), its own endpoint (seid 8), transport, SBC
    const Bytes cfg = {0x04,
                       0x20,
                       0x01,
                       0x00,
                       0x07,
                       0x06,
                       0x00,
                       0x00,
                       static_cast<std::uint8_t>(caps & 0xff),
                       static_cast<std::uint8_t>(caps >> 8 & 0xff),
                       static_cast<std::uint8_t>(caps >> 16 & 0xff),
                       static_cast<std::uint8_t>(caps >> 24)};
    sendCommand(1, 3, cfg, delayMs + 600);
    sendCommand(2, 6, {0x04}, delayMs + 900);
    connectRequest(mediaCid, delayMs + 1200);
    if (driveSendsStart)
    {
        sendCommand(3, 7, {0x04}, delayMs + 1300);
    }
}

void FakeSpeaker::onAcl(const Bytes& p)
{
    if (p.size() < 8)
    {
        return;
    }
    const std::uint16_t cid = p[6] | p[7] << 8;
    const Bytes payload(p.begin() + 8, p.end());

    if (cid == 1)
    {
        onSignaling(payload);
    }
    else if (cid == sigCid)
    {
        onAvdtp(payload);
    }
    else if (cid == mediaCid)
    {
        if (payload.size() >= 13)
        {
            MediaPacket m;
            m.sequence = static_cast<std::uint16_t>(payload[2] << 8 | payload[3]);
            m.timestamp =
                static_cast<std::uint32_t>(payload[4]) << 24 | payload[5] << 16 | payload[6] << 8 | payload[7];
            m.sbcFrames = payload[12];
            m.payloadBytes = static_cast<int>(payload.size()) - 13;
            m.at = m_clock.nowMs();
            media.push_back(m);
        }
    }
    if (reportsCompletions)
    {
        m_controller.completedPackets(m_controller.remote.handle, 1, 5);
    }
}

void FakeSpeaker::onSignaling(const Bytes& c)
{
    if (c.size() < 4)
    {
        return;
    }
    const std::uint8_t code = c[0], id = c[1];
    const Bytes d(c.begin() + 4, c.end());

    switch (code)
    {
    case 0x02: { // the stack connects a channel (psm 0x19)
        const std::uint16_t scid = d[2] | d[3] << 8;
        const std::uint16_t mine = m_channelsAccepted == 0 ? sigCid : mediaCid;
        (m_channelsAccepted == 0 ? m_ourSigCid : m_ourMediaCid) = scid;
        m_channelsAccepted++;
        Bytes r = {0x03, id, 8, 0};
        append(r, u16(mine));
        append(r, u16(scid));
        append(r, {0, 0, 0, 0});
        l2capSig(r);
        Bytes q = {0x04, 0x72, 8, 0};
        append(q, u16(scid));
        append(q, {0, 0, 1, 2});
        append(q, u16(mtu));
        l2capSig(q);
        break;
    }
    case 0x03: {                                       // the stack accepted a channel we asked for
        const std::uint16_t theirs = d[0] | d[1] << 8; // the stack's cid
        const std::uint16_t scid = d[2] | d[3] << 8;   // ours
        (scid == sigCid ? m_ourSigCid : m_ourMediaCid) = theirs;
        Bytes q = {0x04, 0x73, 8, 0}; // now its own configure request
        append(q, u16(theirs));
        append(q, {0, 0, 1, 2});
        append(q, u16(mtu));
        l2capSig(q);
        break;
    }
    case 0x04: { // its configure request: answer
        Bytes r = {0x05, id, 6, 0};
        append(r, {d[0], d[1], 0, 0, 0, 0});
        l2capSig(r);
        break;
    }
    case 0x06: { // it closes a channel
        Bytes r = {0x07, id, 4, 0};
        append(r, d);
        l2capSig(r);
        break;
    }
    default:
        break;
    }
}

void FakeSpeaker::onAvdtp(const Bytes& f)
{
    if (f.size() < 2)
    {
        return;
    }
    const std::uint8_t label = f[0] >> 4, type = f[0] & 3, signal = f[1] & 0x3f;

    if (type != 0)
    { // an answer to a command of ours
        lastAvdtpReply = f;
        if (type == 3)
        {
            rejectedGeneral++;
        }
        return;
    }
    signals.push_back(signal);
    if (refuseRepeats && signal != 1 && signal != 2 && std::count(signals.begin(), signals.end(), signal) > 1)
    {
        avdtp({static_cast<std::uint8_t>(label << 4 | 3), signal, 0x00, 0x13});
        return;
    }
    switch (signal)
    {
    case 1:
        accept(label, 1, hasSbcSink ? Bytes{0x08, 0x08} : Bytes{0x08, 0x00});
        break;
    case 2: {
        Bytes caps = {0x01,
                      0x00,
                      0x07,
                      0x06,
                      0x00,
                      0x00,
                      static_cast<std::uint8_t>(sbcCaps & 0xff),
                      static_cast<std::uint8_t>(sbcCaps >> 8 & 0xff),
                      static_cast<std::uint8_t>(sbcCaps >> 16 & 0xff),
                      static_cast<std::uint8_t>(sbcCaps >> 24)};
        accept(label, 2, caps);
        if (configuresAfterCapabilities)
        {
            configuresAfterCapabilities = false;
            sendCommand(1, 3, {0x04, 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35});
            sendCommand(2, 6, {0x04});
            connectRequest(mediaCid, 50);
        }
        break;
    }
    case 3:
        configuration.assign(f.begin() + 2, f.end());
        if (configuresWhenConfigured)
        {
            configuresWhenConfigured = false;
            sendCommand(1, 3, {0x04, 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35});
            sendCommand(2, 6, {0x04});
            connectRequest(mediaCid, 50);
            avdtp({static_cast<std::uint8_t>(label << 4 | 3), 3, 0x00, 0x31});
        }
        else if (refuseConfiguration)
        {
            avdtp({static_cast<std::uint8_t>(label << 4 | 3), 3, 0x07, 0x13});
        }
        else
        {
            accept(label, 3);
        }
        break;
    case 6:
        accept(label, 6);
        if (opensMediaItself)
        {
            connectRequest(mediaCid, 50);
        }
        break;
    case 7:
        started = true;
        accept(label, 7);
        break;
    case 8:
        closed = true;
        accept(label, 8);
        break;
    default:
        avdtp({static_cast<std::uint8_t>(label << 4 | 3), signal, 0x01});
        break;
    }
}

} // namespace bluev::testing
