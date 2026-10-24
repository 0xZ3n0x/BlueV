// SPDX-License-Identifier: GPL-3.0-or-later
// the HCI event / ACL dispatcher (poll)
#include <cstdio>
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/device_filter.hpp"
#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/bluetooth/sdp_server.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{
constexpr int kPacketMax = 0x44c;
}

void BtStack::poll(std::uint32_t timeoutMs)
{
    std::uint8_t evt[kPacketMax] = {};
    std::uint8_t acl[kPacketMax] = {};
    int n;

    m_transport.pump(timeoutMs);

    while ((n = m_transport.nextEvent(evt, kPacketMax)) > 0)
    {
        handleEvent(evt, n);
    }

    // ACL side: L2CAP reassembly and dispatch
    while ((n = m_transport.nextAcl(acl, kPacketMax)) >= 1)
    {
        handleAcl(acl, n);
    }

    // the once a second callback (setTick)
    if (m_tick)
    {
        const std::uint64_t now = m_clock.nowMs();

        if (now - m_lastTick >= 1000)
        {
            m_lastTick = now;
            m_tick();
        }
    }
}

bool BtStack::isOurLink(const std::uint8_t* e) const
{
    return m_connDone && m_connStatus == 0 && m_handle == ((e[1] | e[2] << 8) & 0xfff);
}

void BtStack::handleEvent(const std::uint8_t* evt, int n)
{
    const std::uint8_t* e = evt + 2; // skip code+len

    switch (evt[0])
    {
    case 0x01: // inquiry complete
        m_inqDone = true;
        break;
    case 0x02: // inquiry result
        for (int i = 0; i < e[0]; i++)
        {
            onInquiryRecord(e + 1 + i * 14, -128, nullptr, 0);
        }
        break;
    case 0x03:
        onConnectionComplete(e);
        break;
    case 0x04:
        onConnectionRequest(e);
        break;
    case 0x05:
        onDisconnectionComplete(e);
        break;
    case 0x06:
        onAuthenticationComplete(e);
        break;
    case 0x08:
        onEncryptionChange(e);
        break;
    case 0x0e:
        onCommandComplete(evt, n);
        break;
    case 0x0f:
        onCommandStatus(e);
        break;
    case 0x13:
        onCompletedPackets(e, n);
        break;
    case 0x16: // PIN code request
        if (std::memcmp(e, m_target.data(), 6) == 0)
        {
            std::uint8_t rep[23];

            logLine("PIN code requested, replying 0000");
            std::memset(rep, 0, sizeof rep);
            std::memcpy(rep, e, 6);
            rep[6] = 4;
            rep[7] = rep[8] = rep[9] = rep[10] = '0';
            cmd(hci::kPinCodeReply, rep, 23);
        }
        break;
    case 0x17: // link key request
        if (std::memcmp(e, m_target.data(), 6) == 0)
        {
            std::uint8_t rep[22];

            if (!m_key.have || std::memcmp(m_key.peer.data(), e, 6) != 0)
            {
                logLine("no stored key: pairing");
                cmd(hci::kLinkKeyNegReply, e, 6);
            }
            else
            {
                std::memcpy(rep, e, 6);
                std::memcpy(rep + 6, m_key.key.data(), 16);
                cmd(hci::kLinkKeyReply, rep, 22);
            }
        }
        break;
    case 0x18:
        onLinkKeyNotification(e);
        break;
    case 0x22: // inquiry result with RSSI
        for (int i = 0; i < e[0]; i++)
        {
            onInquiryRecord(e + 1 + i * 14, static_cast<std::int8_t>(e[14 + i * 14]), nullptr, 0);
        }
        break;
    case 0x2f: // extended inquiry result
        onInquiryRecord(e + 1, static_cast<std::int8_t>(e[14]), e + 15, n - 0x11);
        break;
    case 0x31: // IO capability request
        if (std::memcmp(e, m_target.data(), 6) == 0)
        {
            std::uint8_t rep[9];

            std::memcpy(rep, e, 6);
            rep[6] = 3; // NoInputNoOutput
            rep[7] = 0; // no OOB
            rep[8] = 4; // MITM not required
            cmd(hci::kIoCapabilityReply, rep, 9);
        }
        break;
    case 0x33: // user confirmation request
        if (std::memcmp(e, m_target.data(), 6) == 0)
        {
            cmd(hci::kUserConfirmReply, e, 6);
        }
        break;
    default:
        if (evt[0] == 0xff && m_vendorDumps < 5)
        { // vendor event: raw
            char dump[128];
            int off = 0;
            int m = n - 1;

            m_vendorDumps++;
            if (m > 0x1f)
            {
                m = 0x1f;
            }
            for (int k = 0; k < m + 1; k++)
            {
                off += std::snprintf(dump + off, 0x61 - off, "%02x ", evt[k]);
            }
            logLine("chip vendor event, raw (%d bytes): %s", n, dump);
        }
        break;
    }
}

void BtStack::onConnectionComplete(const std::uint8_t* e)
{
    const BdAddr peer = addressFrom(e + 3);

    // service mode: a paired device that dialed in while we were busy with
    // something else still counts
    if (m_service && e[0] == 0 && !m_connDone && peer != m_target && bondExists(peer))
    {
        m_target = peer;
        setBond(peer);
    }
    if (peer == m_target && (e[0] != 0x0b || !m_connDone))
    {
        m_connDone = true;
        m_handle = (e[1] | e[2] << 8) & 0xfff;
        m_connStatus = e[0];
        if (e[0] == 0)
        { // a fresh link: clean slate
            m_connGen++;
            m_disconnected = false;
            m_authDone = false;
            m_authStatus = 0;
            m_encOn = false;
            m_encStatus = 0;
            m_credits = m_creditsMax;
            m_ifHead = 0;
            m_ifCount = 0;
        }
        if (e[0] == 0)
        {
            logLine("connection complete: handle %#05x", m_handle);
        }
        else
        {
            logLine("connection complete: %s (status %#04x)", hciStatusName(e[0]), e[0]);
        }
    }
}

void BtStack::onConnectionRequest(const std::uint8_t* e)
{
    const BdAddr peer = addressFrom(e);
    bool accept;

    if (e[9] != 0x01) // ACL only
    {
        return;
    }
    if ((m_key.have && m_key.peer == peer) || peer == m_target)
    {
        m_target = peer;
        accept = true;
    }
    else if (m_service)
    {
        // service mode: only devices paired before dial in
        const std::uint32_t cls = static_cast<std::uint32_t>(e[6]) | e[7] << 8 | e[8] << 16;

        if (bondExists(peer))
        {
            m_target = peer;
            setBond(peer);
            accept = true;
        }
        else
        {
            const long count = m_foreignRequests.fetch_add(1) + 1;

            logLine("inbound connection from %s (%s, class %06x) "
                    "ignored: not paired with this adapter, #%ld",
                    addressText(peer).c_str(), deviceClassName(cls), cls, count);
            accept = false;
        }
    }
    else
    {
        const std::uint32_t cls = static_cast<std::uint32_t>(e[6]) | e[7] << 8 | e[8] << 16;

        if (!m_key.have && (e[7] & 0x1f) == 4 && !isJunkDevice(cls, std::string()))
        {
            m_target = peer;
            accept = true; // unknown headphones dialing in
        }
        else
        {
            accept = false;
        }
    }
    if (accept)
    {
        std::uint8_t rep[7];

        logLine("the device is connecting to us by itself");
        std::memcpy(rep, e, 6);
        // Role: 0 = become the master of this link, 1 = stay the slave. As the
        // slave of the speaker's link while being the master of the console's
        // controllers, the chip has to share its time between two piconets.
        rep[6] = 0;
        cmd(hci::kAcceptConnReq, rep, 7);
    }
}

void BtStack::onDisconnectionComplete(const std::uint8_t* e)
{
    if (isOurLink(e))
    {
        logLine("disconnected: %s (reason %#04x)", hciStatusName(e[3]), e[3]);
        m_disconnected = true;
    }
    else
    {
        logLine("another link closed: handle %#05x, %s (reason %#04x)", (e[1] | e[2] << 8) & 0xfff, hciStatusName(e[3]),
                e[3]);
    }
}

void BtStack::onAuthenticationComplete(const std::uint8_t* e)
{
    if (!isOurLink(e))
    {
        return;
    }
    m_authDone = true;
    m_authStatus = e[0];
    if (e[0] == 0)
    {
        logLine("authentication complete");
    }
    else
    {
        logLine("authentication failed: %s (status %#04x)", hciStatusName(e[0]), e[0]);
    }
}

void BtStack::onEncryptionChange(const std::uint8_t* e)
{
    if (!isOurLink(e))
    {
        return;
    }
    m_encStatus = e[0];
    if (m_encStatus == 0)
    {
        m_encOn = e[3] != 0;
    }
    logLine("encryption change: %s, %s", hciStatusName(e[0]), e[3] ? "on" : "off");
}

void BtStack::onCommandComplete(const std::uint8_t* evt, int n)
{
    const std::uint8_t* e = evt + 2;
    const std::uint16_t op = static_cast<std::uint16_t>(e[1] | e[2] << 8);

    if (!replyIsOurs(op) && m_systemReplies < 50)
    {
        m_systemReplies++;
        logLine("the console's own command answered: %s (%#06x), status %#04x", hciOpcodeName(op), op, e[3]);
    }
    if (op == hci::kRoleDiscovery && e[3] == 0)
    {
        logLine("link role: handle %#05x, we are the %s", (e[4] | e[5] << 8) & 0xfff, e[6] == 0 ? "master" : "slave");
    }
    m_ccOp = op;
    m_ccLen = n > 1099 ? 1100 : static_cast<std::uint32_t>(n);
    std::memcpy(m_cc, evt, m_ccLen);
}

void BtStack::onCommandStatus(const std::uint8_t* e)
{
    const std::uint16_t op = static_cast<std::uint16_t>(e[2] | e[3] << 8);
    const std::uint32_t status = e[0];

    if (!replyIsOurs(op) && m_systemReplies < 50)
    {
        m_systemReplies++;
        logLine("the console's own command answered: %s (%#06x), status %#04x", hciOpcodeName(op), op, e[0]);
    }
    if (status != 0)
    {
        if (op == hci::kAuthRequested)
        {
            m_authDone = true;
            m_authStatus = static_cast<int>(status);
        }
        else if (op == hci::kCreateConn)
        {
            m_connDone = true;
            m_connStatus = static_cast<int>(status);
        }
    }
}

void BtStack::onCompletedPackets(const std::uint8_t* e, int n)
{
    for (int i = 0; i < e[0]; i++)
    {
        const std::uint8_t* r = e + 1 + i * 4;

        if (4 * i > n - 7)
        {
            break;
        }
        const std::uint16_t h = static_cast<std::uint16_t>(r[0] | r[1] << 8);
        const std::uint16_t cnt = static_cast<std::uint16_t>(r[2] | r[3] << 8);

        if (m_connStatus == 0 && m_connDone && m_handle == (h & 0xfff))
        {
            m_credits += cnt;
            m_reported += cnt;
            for (int j = 0; j < cnt; j++)
            {
                if (m_ifCount == 0)
                {
                    break;
                }
                m_ifHead = (m_ifHead + 1) % 64;
                m_ifCount--;
            }
        }
        else
        {
            m_otherCompleted += cnt;
        }
    }
    if (m_creditsMax != 0 && m_credits > m_creditsMax)
    {
        m_credits = m_creditsMax;
    }
}

void BtStack::onLinkKeyNotification(const std::uint8_t* e)
{
    bool junk = false;

    if (std::memcmp(e, m_target.data(), 6) != 0)
    {
        return;
    }
    m_key.peer = addressFrom(e);
    std::memcpy(m_key.key.data(), e + 6, 16);
    m_key.type = e[22];
    m_key.have = true;

    for (const BtDevice& d : m_devices)
    {
        if (d.address == m_key.peer)
        {
            junk = isJunkDevice(d.classOfDevice, d.name);
            break;
        }
    }
    if (junk)
    {
        logLine("not saving key for TV/speaker %s", addressText(m_key.peer).c_str());
        m_bondStore.removeKey(m_local, m_key.peer);
        m_key = BondKey{};
        return;
    }
    if (!m_bondStore.saveKey(m_local, m_key))
    {
        logLine("link key: cannot write the key of %s", addressText(m_key.peer).c_str());
        return;
    }
    logLine("paired with %s, key saved", addressText(m_key.peer).c_str());
}

void BtStack::handleAcl(const std::uint8_t* acl, int n)
{
    const std::uint16_t hf = static_cast<std::uint16_t>(acl[0] | acl[1] << 8);
    const std::uint16_t dlen = static_cast<std::uint16_t>(acl[2] | acl[3] << 8);
    const std::uint8_t* pl = acl + 4;

    // The event and ACL pipes are separate, so an ACL packet can arrive before
    // the connection complete event; until a link of ours is known every
    // handle is accepted. Say so when it happens, because the console's own
    // links (game controllers) share this controller and their packets must
    // never be mistaken for ours.
    if (!m_connDone && m_otherAclLogged < 20)
    {
        m_otherAclLogged++;
        logLine("ACL from handle %#05x (len %u) before a link of ours", hf & 0xfff, dlen);
    }
    if ((hf & 0xfff) != m_handle && m_connDone)
    {
        m_otherAcl++;
    }
    if ((!m_connDone || (hf & 0xfff) == m_handle) && dlen + 4 <= static_cast<std::uint32_t>(n))
    {
        if (!m_reassembler.feed(hf, pl, dlen))
        {
            return;
        }

        const std::uint16_t cid = m_reassembler.cid();
        const std::uint8_t* payload = m_reassembler.payload();
        const int len = m_reassembler.payloadLength();

        if (cid == 1)
        {
            signaling(payload, len);
        }
        else if (cid == 0x50)
        {
            std::uint8_t out[512];
            const int r = SdpServer::handle(payload, static_cast<std::uint32_t>(len), out, 512);

            if (r > 0 && m_sdp.theirCid != 0)
            {
                send(m_sdp.theirCid, out, r);
            }
        }
        else
        {
            dispatch(cid, payload, len);
        }
        m_reassembler.release();
    }
}

} // namespace bluev
