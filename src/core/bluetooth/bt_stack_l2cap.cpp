// SPDX-License-Identifier: GPL-3.0-or-later
// L2CAP channels and the signaling command handler
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bytes.hpp"
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
    return send(1, pkt, static_cast<int>(p - pkt));
}

bool BtStack::openChannel(L2Channel& ch, std::uint16_t psm, FrameHandler handler)
{
    const std::uint32_t ourCid = ch.ourCid;
    int retries = 0;

    addChannel(ch, std::move(handler));

    for (;;)
    {
        ch.connDone = false;
        if (!sendSignal(0x02, m_sigId++, {psm, ourCid})) // connection request
        {
            return false;
        }

        if (!wait([&ch] { return ch.connDone; }, 10000))
        {
            logLine("l2cap: %s connection timed out", ch.name);
            return false;
        }
        if (ch.result == 4 && retries++ < 6)
        {
            // "no resources": the peer is still busy, ask again shortly
            logLine("l2cap: %s: peer busy, asking again", ch.name);
            poll(2000);
            if (ch.up && ch.configured)
            {
                return true; // the peer opened it meanwhile
            }
            continue;
        }
        break;
    }
    if (ch.result != 0)
    {
        logLine("l2cap: %s connection refused (%d)", ch.name, static_cast<int>(ch.result));
        return false;
    }
    ch.up = false;

    // keep sending configure requests until the channel is up
    std::uint64_t cfgDeadline = 0;
    const std::uint64_t deadline = m_clock.nowMs() + 14000;
    std::uint64_t upSince = 0;
    for (;;)
    {
        if (ch.up && ch.configured)
        {
            return true;
        }
        // some headsets never send their own configure request; go on anyway
        // after a grace period
        if (ch.up)
        {
            if (upSince == 0)
            {
                upSince = m_clock.nowMs();
            }
            else if (m_clock.nowMs() - upSince >= 8000)
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
        if (!ch.up && m_clock.nowMs() >= cfgDeadline)
        {
            // configure request: our channel, no flags, option MTU (type 1, length 2)
            if (!sendSignal(0x04, m_sigId++, {ch.theirCid, 0, 0x0201, 672}))
            {
                return false;
            }
            cfgDeadline = m_clock.nowMs() + 6000;
        }
        poll(100);
    }
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
    m_inPsm = psm;
    m_inChannel = &ch;
    m_inHandler = std::move(handler);
}

// L2CAP signaling command handler (cid 1)
void BtStack::signaling(const std::uint8_t* cmd, int len)
{
    int off = 0;

    while (off + 4 <= len)
    {
        const std::uint8_t code = cmd[off];
        const std::uint8_t id = cmd[off + 1];
        const std::uint16_t clen = getLe16(cmd + off + 2);
        const std::uint8_t* d = cmd + off + 4;
        std::uint8_t rsp[280];
        L2Channel* ch = nullptr;

        if (off + 4 + clen > len)
        {
            break;
        }
        // the fixed part of each command the code below reads, or answers with
        if (clen < minimumLength(code))
        {
            off += 4 + clen;
            continue;
        }

        std::memset(rsp, 0, sizeof rsp);
        switch (code)
        {
        case 0x02: { // connection request
            const std::uint16_t psm = getLe16(d);
            const std::uint16_t scid = getLe16(d + 2);
            std::uint16_t our;
            bool sdpIn = false;

            if (psm == 0x0001)
            { // SDP: always accept, cid 0x50
                m_sdp.accept(scid);
                our = 0x50;
                sdpIn = true;
            }
            else if (m_inChannel == nullptr || m_inPsm != psm)
            {
                // a device keeps asking for the remote control channel every
                // few seconds; say it twice per link, not every time
                if (m_rejectedGen != m_connGen)
                {
                    m_rejectedGen = m_connGen;
                    m_rejectedCount = 0;
                }
                if (m_rejectedCount++ < 2)
                {
                    logLine("l2cap: rejected psm %#x%s", psm, psm == 0x17 ? " (remote control, not offered)" : "");
                }
                sendSignal(0x03, id, {0, scid, 2, 0}); // connection response: no channel, PSM denied
                break;
            }
            else
            {
                m_inChannel->accept(scid);
                addChannel(*m_inChannel, m_inHandler);
                our = static_cast<std::uint16_t>(m_inChannel->ourCid);
            }
            sendSignal(0x03, id, {our, scid, 0, 0}); // connection response: successful
            sendSignal(0x04, m_sigId++, {scid, 0});  // configure request, no flags
            if (!sdpIn)
            {
                logLine("l2cap: accepted inbound psm %#x", m_inPsm);
            }
            break;
        }
        case 0x03: // connection response
            ch = channelByTheirs(getLe16(d + 2));
            if (ch != nullptr && getLe16(d + 4) != 1)
            { // not pending
                ch->theirCid = getLe16(d);
                ch->result = getLe16(d + 4);
                ch->connDone = true;
            }
            break;
        case 0x04: // configure request
            ch = channelByTheirs(getLe16(d));
            if (ch != nullptr)
            {
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
            break;
        case 0x05: // configure response
            ch = channelByTheirs(getLe16(d));
            if (ch == nullptr)
            {
                // response for a channel opened from our side: take the
                // newest one that is connecting but not up yet
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
            break;
        case 0x06: // disconnection request
            ch = channelByTheirs(getLe16(d));
            sendSignal(0x07, id, {getLe16(d), getLe16(d + 2)}); // disconnection response
            if (ch != nullptr)
            {
                ch->closed = true;
                if (ch == &m_sdp)
                {
                    m_sdp.theirCid = 0;
                }
            }
            break;
        case 0x07: // disconnection response
            ch = channelByTheirs(getLe16(d + 2));
            if (ch != nullptr)
            {
                ch->closed = true;
            }
            break;
        case 0x08:                     // echo request
            if (clen > sizeof rsp - 4) // more than the answer can carry
            {
                break;
            }
            rsp[0] = 0x09;
            rsp[1] = id;
            putLe16(rsp + 2, clen);
            if (clen != 0)
            {
                std::memcpy(rsp + 4, d, clen);
            }
            send(1, rsp, clen + 4);
            break;
        case 0x0a: { // information request
            const std::uint16_t type = getLe16(d);

            rsp[0] = 0x0b;
            rsp[1] = id;
            rsp[4] = d[0];
            rsp[5] = d[1];
            if (type == 3)
            {
                rsp[2] = 0x0c;
                rsp[3] = 0;
                rsp[6] = 0;
                rsp[7] = 0; // result: successful
                rsp[8] = 2;
                rsp[9] = 0; // features: connectionless
                send(1, rsp, 16);
            }
            else if (type == 2)
            {
                rsp[2] = 8;
                rsp[3] = 0; // success, empty feature mask
                send(1, rsp, 12);
            }
            else
            {
                rsp[2] = 4;
                rsp[3] = 0;
                rsp[6] = 1;
                rsp[7] = 0; // not supported
                send(1, rsp, 8);
            }
            break;
        }
        default:
            break;
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
