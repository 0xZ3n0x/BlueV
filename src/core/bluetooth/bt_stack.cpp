// SPDX-License-Identifier: GPL-3.0-or-later
// command helpers, waiting, bonds, the ACL send path and flow control
#include "core/bluetooth/bt_stack.hpp"

#include <cstring>

#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

BtStack::BtStack(IHciTransport& transport, IBondStore& bonds, IUsbDirectory& usb, IFirmwareStore& firmware,
                 IClock& clock)
    : m_transport(transport), m_bondStore(bonds), m_usb(usb), m_firmware(firmware), m_clock(clock)
{
}

// ------------------------------------------------------------------
// commands
// ------------------------------------------------------------------
bool BtStack::isTarget(const std::uint8_t* address) const
{
    return std::memcmp(address, m_target.data(), 6) == 0;
}

void BtStack::clearConnectResult()
{
    m_connDone = false;
    m_connStatus = 0;
}

void BtStack::clearSecurity()
{
    m_authDone = false;
    m_authStatus = 0;
    m_encOn = false;
    m_encStatus = 0;
}

void BtStack::resetCredits()
{
    m_credits = m_creditsMax;
    m_ifHead = 0;
    m_ifCount = 0;
}

void BtStack::dropAllLinks(std::uint32_t pauseMs)
{
    // on the console's chip these handles can be the gamepad's
    if (!m_external)
    {
        return;
    }
    for (int h = 1; h <= 8; h++)
    {
        sendDisconnect(static_cast<std::uint16_t>(h));
        poll(pauseMs);
    }
}

void BtStack::sendDisconnect(std::uint16_t handle)
{
    std::uint8_t disc[3];

    putLe16(disc, handle);
    disc[2] = 0x13;
    cmd(hci::kDisconnect, disc, 3);
}

bool BtStack::cmd(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length)
{
    if (!m_transport.command(opcode, params, length))
    {
        return false;
    }

    // remember it, so that the answers to the console's own commands can be told apart
    int oldest = 0;
    for (int i = 0; i < kSentSlots; i++)
    {
        if (m_sentCommands[i].op == 0)
        {
            oldest = i;
            break;
        }
        if (m_sentCommands[i].at < m_sentCommands[oldest].at)
        {
            oldest = i;
        }
    }
    m_sentCommands[oldest].op = opcode;
    m_sentCommands[oldest].at = m_clock.nowMs();
    return true;
}

// Is this reply to a command we sent? Each sent command is matched once, within 20 s.
bool BtStack::replyIsOurs(std::uint16_t opcode)
{
    const std::uint64_t now = m_clock.nowMs();

    if (opcode == 0)
    {
        return true; // a bare "commands allowed" update
    }
    for (SentCommand& s : m_sentCommands)
    {
        if (s.op == opcode && now - s.at < 20000)
        {
            s.op = 0;
            return true;
        }
    }
    return false;
}

bool BtStack::hciSync(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length, int attempts)
{
    const int tries = attempts < 1 ? 1 : attempts;

    for (int attempt = 1;; attempt++)
    {
        m_ccOp = 0;
        if (!cmd(opcode, params, length))
        {
            return false;
        }
        // later tries wait longer: a controller that just woke up can be slow
        if (wait([this, opcode] { return m_ccOp == opcode; }, attempt < tries - 1 ? 1500 : 2500, false))
        {
            break;
        }
        if (attempt >= tries)
        {
            logLine("no reply to %s (%#06x)", hciOpcodeName(opcode), opcode);
            const std::string why = m_transport.describe("no-reply");
            if (!why.empty())
            {
                logLine("%s", why.c_str());
            }
            return false;
        }
    }
    return m_cc[5] == 0 && m_ccLen > 5;
}

bool BtStack::wait(const std::function<bool()>& done, std::uint32_t timeoutMs, bool stopOnLinkLoss)
{
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;

    while (!done())
    {
        std::int64_t left = static_cast<std::int64_t>(deadline - m_clock.nowMs());

        if (left < 1)
        {
            return false;
        }
        if (left > 100)
        {
            left = 100;
        }
        poll(static_cast<std::uint32_t>(left));
        if (stopOnLinkLoss && m_disconnected)
        {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------
// bonds: the link key belongs to the pair (our adapter, the device)
// ------------------------------------------------------------------
bool BtStack::hasBond(const BdAddr& peer)
{
    return m_localKnown && m_bondStore.hasKey(m_local, peer);
}

// Makes `peer` the device whose key and name are in use (empty when unknown).
void BtStack::setBond(const BdAddr& peer)
{
    m_key = BondKey{};
    m_bondStore.loadKey(m_local, peer, m_key);
    m_peerName = m_bondStore.loadName(peer);
}

std::vector<BondEntry> BtStack::bonds()
{
    if (!m_localKnown)
    {
        return {};
    }
    return m_bondStore.list(m_local);
}

// forgets the device for every adapter
void BtStack::forget(const BdAddr& peer)
{
    m_bondStore.forget(peer);
    if (m_key.have && m_key.peer == peer)
    {
        m_key = BondKey{};
    }
    logLine("forgot %s", addressText(peer).c_str());
}

// ------------------------------------------------------------------
// ACL send path and credit-based flow control
// ------------------------------------------------------------------
bool BtStack::send(std::uint32_t cid, const std::uint8_t* data, int len)
{
    std::uint8_t pkt[4 + 1096];
    const int total = len + 4;

    if (len >= 0x445 || (m_aclMtu != 0 && total > static_cast<int>(m_aclMtu)))
    {
        logLine("l2cap: %d-byte frame too large to send unfragmented", len);
        return false;
    }

    putLe16(pkt, (m_handle & 0x0fff) | 0x2000); // PB: start, no flush
    putLe16(pkt + 2, total);
    putLe16(pkt + 4, len);
    putLe16(pkt + 6, cid);
    std::memcpy(pkt + 8, data, len);

    if (!m_transport.sendAcl(pkt, len + 8))
    {
        return false;
    }

    m_credits--;
    m_sent++;

    pushInFlight(m_clock.nowMs());
    return true;
}

// the ring of send times; when it is full the oldest entry is dropped
void BtStack::pushInFlight(std::uint64_t sentAt)
{
    if (m_ifCount == kInFlightSlots)
    {
        popInFlight();
    }
    m_inflight[(m_ifCount + m_ifHead) % kInFlightSlots] = sentAt;
    m_ifCount++;
}

void BtStack::popInFlight()
{
    m_ifHead = (m_ifHead + 1) % kInFlightSlots;
    m_ifCount--;
}

// Credits > 0 and at most 5 unconfirmed packets in flight. A packet with no
// completion report after 31 ms is assumed complete (completionsAssumed()).
//
// Limits of 3 unconfirmed packets and 100 ms would cap the stream near 40
// packets/s. A 44.1/48 kHz stream needs 49-53, and on the Slim's controller
// that dropped about 20 ms of audio every 10 s (measured: trimmed 5376 per
// minute with those limits, 0 with the values above).
bool BtStack::canSend()
{
    const std::uint64_t now = m_clock.nowMs();
    bool ok = true;

    if (m_ifCount > 5)
    {
        do
        {
            if (now - m_inflight[m_ifHead] < 31)
            {
                break;
            }
            popInFlight();
            if (m_credits < m_creditsMax)
            {
                m_credits++;
            }
            m_assumed++;
        } while (m_ifCount > 5);
        ok = m_ifCount < 6;
    }
    return m_credits > 0 && ok;
}

} // namespace bluev
