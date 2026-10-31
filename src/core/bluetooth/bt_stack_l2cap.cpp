// SPDX-License-Identifier: GPL-3.0-or-later
// L2CAP channels and the signaling command handler
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bytes.hpp"
#include "core/hex.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{

// The least a signaling command of this code carries after its 4 byte header:
// connection request 4, connection response 8, configure request 4, configure
// response 6, disconnection request and response 4, information request 2.
// Other codes are not read.
std::uint16_t minimumLength(std::uint8_t code)
{
    switch (code)
    {
    case 0x02:
        return 4;
    case 0x03:
        return 8;
    case 0x04:
        return 4;
    case 0x05:
        return 6;
    case 0x06:
        return 4;
    case 0x07:
        return 4;
    case 0x0a:
        return 2;
    }
    return 0;
}

} // namespace

// Channels that get frames: a new one is added once, the SDP channel never
// (it is served by the stack itself). At most kMaxChannels.
void BtStack::addChannel(L2Channel& ch, FrameHandler handler)
{
    if (m_channelCount < kMaxChannels && ch.ourCid != 0x50)
    {
        for (int i = 0; i < m_channelCount; i++)
        {
            if (m_channels[i]->ourCid == ch.ourCid)
            {
                return;
            }
        }
        m_channels[m_channelCount] = &ch;
        m_handlers[m_channelCount] = std::move(handler);
        m_channelCount++;
    }
}

L2Channel* BtStack::channelByTheirs(std::uint16_t cid)
{
    if (cid == 0x50)
    {
        return &m_sdp;
    }
    for (int i = 0; i < m_channelCount; i++)
    {
        if (m_channels[i]->ourCid == cid)
        {
            return m_channels[i];
        }
    }
    return nullptr;
}

bool BtStack::sendSignal(std::uint8_t code, std::uint8_t id, std::initializer_list<unsigned> fields)
{
    std::uint8_t pkt[4 + 2 * 4];
    std::uint8_t* p = pkt + 4;

    if (fields.size() > 4)
    {
        return false;
    }
    pkt[0] = code;
    pkt[1] = id;
    putLe16(pkt + 2, static_cast<unsigned>(2 * fields.size()));
    for (const unsigned field : fields)
    {
        putLe16(p, field);
        p += 2;
    }
    if (!m_external)
    {
        logLine("l2cap: > %s", hexStart(pkt, static_cast<int>(p - pkt)).c_str());
    }
    return send(1, pkt, static_cast<int>(p - pkt));
}

// Asks for the channel until the device answers; a busy device is asked again
// shortly.
BtStack::Request BtStack::requestChannel(L2Channel& ch, std::uint16_t psm)
{
    int retries = 0;

    for (;;)
    {
        ch.connDone = false;
        if (!sendSignal(0x02, m_sigId++, {psm, ch.ourCid})) // connection request
        {
            return Request::Failed;
        }
        if (!wait([&ch] { return ch.connDone; }, 10000))
        {
            logLine("l2cap: %s connection timed out", ch.name);
            return Request::Failed;
        }
        if (ch.result != 4 || retries++ >= 6)
        {
            break;
        }
        // "no resources": the peer is still busy, ask again shortly
        logLine("l2cap: %s: peer busy, asking again", ch.name);
        poll(2000);
        if (ch.up && ch.configured)
        {
            return Request::OpenedByPeer;
        }
    }
    if (ch.result != 0)
    {
        logLine("l2cap: %s connection refused (%d)", ch.name, static_cast<int>(ch.result));
        return Request::Failed;
    }
    return Request::Accepted;
}

// Keeps sending configure requests until the channel is up and the device has
// configured its side too. On the console's chip the answer can be slow or go
// to the console: the request is repeated with the same id (a new id reads as a
// second configuration), not at all once the device has sent its own, and then
// ours is taken as accepted.
bool BtStack::configureChannel(L2Channel& ch)
{
    std::uint64_t cfgDeadline = 0;
    std::uint8_t cfgId = 0;
    const std::uint64_t deadline = m_clock.nowMs() + 14000;
    std::uint64_t upSince = 0;

    ch.up = false;
    for (;;)
    {
        if (ch.up && ch.configured)
        {
            return true;
        }
        if (!m_external && !ch.up && ch.configured && cfgDeadline != 0 && m_clock.nowMs() >= cfgDeadline)
        {
            logLine("l2cap: %s: no configure response seen, the device configured, taking ours as accepted", ch.name);
            ch.up = true;
            continue;
        }
        // some headsets never send their own configure request; go on anyway
        // after a grace period
        if (ch.up)
        {
            if (upSince == 0)
            {
                upSince = m_clock.nowMs();
            }
            else if (m_clock.nowMs() - upSince >= (m_external ? 8000u : 3000u))
            {
                logLine("l2cap: %s: no configure request from the peer, continuing anyway", ch.name);
                return true;
            }
        }
        if (m_clock.nowMs() >= deadline || m_disconnected)
        {
            logLine("l2cap: %s configuration did not finish", ch.name);
            return false;
        }
        if (!ch.up && m_clock.nowMs() >= cfgDeadline && (m_external || !ch.configured || cfgDeadline == 0))
        {
            cfgId = (m_external || cfgDeadline == 0) ? m_sigId++ : cfgId;
            // configure request: our channel, no flags, option MTU (type 1, length 2)
            if (!sendSignal(0x04, cfgId, {ch.theirCid, 0, 0x0201, 672}))
            {
                return false;
            }
            cfgDeadline = m_clock.nowMs() + (m_external ? 6000 : 4000);
        }
        poll(100);
    }
}

bool BtStack::openChannel(L2Channel& ch, std::uint16_t psm, FrameHandler handler)
{
    addChannel(ch, std::move(handler));

    switch (requestChannel(ch, psm))
    {
    case Request::Accepted:
        if (!configureChannel(ch))
        {
            return false;
        }
        return m_external || ch.configured || reopenChannel(ch, psm);
    case Request::OpenedByPeer:
        return true;
    case Request::Failed:
        break;
    }
    return false;
}

// The device's configure request went to the console, so the channel stays half
// open on its side and it ignores what we send. Asking for it again, once, makes
// the device send a new one.
bool BtStack::reopenChannel(L2Channel& ch, std::uint16_t psm)
{
    logLine("l2cap: %s: opening it again", ch.name);
    closeChannel(ch);
    ch.closed = false;
    if (m_disconnected)
    {
        return false;
    }
    const Request again = requestChannel(ch, psm);

    return again == Request::OpenedByPeer || (again == Request::Accepted && configureChannel(ch));
}

void BtStack::closeChannel(L2Channel& ch)
{
    if (ch.theirCid == 0 || ch.closed || m_disconnected)
    {
        return;
    }

    sendSignal(0x06, m_sigId++, {ch.theirCid, ch.ourCid}); // disconnection request

    wait([&ch] { return ch.closed; }, 2000);
}

void BtStack::bindInbound(std::uint16_t psm, L2Channel& ch, FrameHandler handler)
{
    // the slot of this PSM, else a free one
    Inbound* slot = m_inbound[0].psm == psm || m_inbound[0].psm == 0 ? &m_inbound[0] : &m_inbound[1];

    if (m_inbound[1].psm == psm)
    {
        slot = &m_inbound[1];
    }
    slot->psm = psm;
    slot->channel = &ch;
    slot->handler = std::move(handler);
}

// The device wants a channel to one of our PSMs: SDP is always accepted, the
// PSM bound with bindInbound() when it matches, anything else is refused.
void BtStack::onChannelRequest(std::uint8_t id, const std::uint8_t* d)
{
    const std::uint16_t psm = getLe16(d);
    const std::uint16_t scid = getLe16(d + 2);
    std::uint16_t our;
    bool sdpIn = false;
    Inbound* bound = nullptr;

    for (Inbound& in : m_inbound)
    {
        if (in.channel != nullptr && in.psm == psm)
        {
            bound = &in;
        }
    }

    if (psm == 0x0001)
    { // SDP: always accept, cid 0x50
        m_sdp.accept(scid);
        our = 0x50;
        sdpIn = true;
    }
    else if (bound == nullptr)
    {
        // a device keeps asking for a channel every few seconds; say it twice
        // per link, not every time
        if (m_rejectedGen != m_connGen)
        {
            m_rejectedGen = m_connGen;
            m_rejectedCount = 0;
        }
        if (m_rejectedCount++ < 2)
        {
            logLine("l2cap: rejected psm %#x", psm);
        }
        sendSignal(0x03, id, {0, scid, 2, 0}); // connection response: no channel, PSM denied
        return;
    }
    else
    {
        bound->channel->accept(scid);
        addChannel(*bound->channel, bound->handler);
        our = static_cast<std::uint16_t>(bound->channel->ourCid);
    }
    sendSignal(0x03, id, {our, scid, 0, 0}); // connection response: successful
    sendSignal(0x04, m_sigId++, {scid, 0});  // configure request, no flags
    if (!sdpIn)
    {
        logLine("l2cap: accepted inbound psm %#x", psm);
    }
}

// the answer to a channel we asked for
void BtStack::onChannelResponse(const std::uint8_t* d)
{
    L2Channel* ch = channelByTheirs(getLe16(d + 2));

    if (ch != nullptr && getLe16(d + 4) != 1)
    { // not pending
        ch->theirCid = getLe16(d);
        ch->result = getLe16(d + 4);
        ch->connDone = true;
    }
}

void BtStack::onConfigureRequest(std::uint8_t id, const std::uint8_t* d, std::uint16_t clen)
{
    L2Channel* ch = channelByTheirs(getLe16(d));

    if (ch == nullptr)
    {
        return;
    }
    // pick up the MTU option (type 1, len 2)
    if (clen > 5)
    {
        std::uint32_t o = 4;

        do
        {
            if ((d[o] & 0x7f) == 1 && d[o + 1] == 2 && o + 4 <= clen)
            {
                ch->theirMtu = getLe16(d + o + 2);
            }
            o += 2 + d[o + 1];
        } while (o + 2 <= clen);
    }
    sendSignal(0x05, id, {ch->theirCid, 0, 0}); // configure response: successful
    ch->configured = true;
}

void BtStack::onConfigureResponse(const std::uint8_t* d)
{
    L2Channel* ch = channelByTheirs(getLe16(d));

    if (ch == nullptr)
    {
        // response for a channel opened from our side: take the newest one
        // that is connecting but not up yet
        for (int i = m_channelCount; i >= 1; i--)
        {
            L2Channel* c = m_channels[i - 1];

            if (!c->connDone || c->up)
            {
                continue;
            }
            ch = c;
            break;
        }
        if (ch == nullptr && m_channelCount > 0)
        {
            ch = m_channels[0];
        }
    }
    if (ch != nullptr && getLe16(d + 4) == 0)
    {
        ch->up = true;
    }
}

void BtStack::onChannelClose(std::uint8_t id, const std::uint8_t* d)
{
    L2Channel* ch = channelByTheirs(getLe16(d));

    sendSignal(0x07, id, {getLe16(d), getLe16(d + 2)}); // disconnection response
    if (ch != nullptr)
    {
        ch->closed = true;
        if (ch == &m_sdp)
        {
            m_sdp.theirCid = 0;
        }
    }
}

void BtStack::onEchoRequest(std::uint8_t id, const std::uint8_t* d, std::uint16_t clen)
{
    std::uint8_t rsp[280];

    if (clen > sizeof rsp - 4) // more than the answer can carry
    {
        return;
    }
    rsp[0] = 0x09;
    rsp[1] = id;
    putLe16(rsp + 2, clen);
    if (clen != 0)
    {
        std::memcpy(rsp + 4, d, clen);
    }
    send(1, rsp, clen + 4);
}

void BtStack::onInformationRequest(std::uint8_t id, const std::uint8_t* d)
{
    const std::uint16_t type = getLe16(d);

    if (type == 3)
    {
        // fixed channels: connectionless; the 12 bytes of the answer are the
        // type, the result (success) and an 8-byte mask padded with zeros
        std::uint8_t rsp[16] = {0x0b, id, 0x0c, 0, d[0], d[1], 0, 0, 2};

        send(1, rsp, sizeof rsp);
    }
    else if (type == 2)
    {
        sendSignal(0x0b, id, {type, 0, 0, 0}); // success, empty feature mask
    }
    else
    {
        sendSignal(0x0b, id, {type, 1}); // not supported
    }
}

// L2CAP signaling command handler (cid 1): one or more commands in a frame
void BtStack::signaling(const std::uint8_t* cmd, int len)
{
    int off = 0;

    while (off + 4 <= len)
    {
        const std::uint8_t code = cmd[off];
        const std::uint8_t id = cmd[off + 1];
        const std::uint16_t clen = getLe16(cmd + off + 2);
        const std::uint8_t* d = cmd + off + 4;

        if (off + 4 + clen > len)
        {
            break;
        }
        if (!m_external)
        {
            logLine("l2cap: < %s", hexStart(cmd + off, 4 + clen).c_str());
        }
        // the fixed part of each command the code below reads, or answers with
        if (clen >= minimumLength(code))
        {
            switch (code)
            {
            case 0x02:
                onChannelRequest(id, d);
                break;
            case 0x03:
                onChannelResponse(d);
                break;
            case 0x04:
                onConfigureRequest(id, d, clen);
                break;
            case 0x05:
                onConfigureResponse(d);
                break;
            case 0x06:
                onChannelClose(id, d);
                break;
            case 0x07: // disconnection response
                if (L2Channel* ch = channelByTheirs(getLe16(d + 2)))
                {
                    ch->closed = true;
                }
                break;
            case 0x08:
                onEchoRequest(id, d, clen);
                break;
            case 0x0a:
                onInformationRequest(id, d);
                break;
            default:
                break;
            }
        }
        off += 4 + clen;
    }
}

void BtStack::dispatch(std::uint32_t cid, const std::uint8_t* buf, int len)
{
    for (int i = 0; i < m_channelCount; i++)
    {
        if (m_channels[i]->ourCid == cid && m_handlers[i])
        {
            m_handlers[i](cid, buf, len);
        }
    }
}

} // namespace bluev
