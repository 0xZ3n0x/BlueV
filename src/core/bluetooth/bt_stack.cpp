// SPDX-License-Identifier: GPL-3.0-or-later
// controller bring-up and shutdown, command helpers, bonds, flow control
#include "core/bluetooth/bt_stack.hpp"

#include <algorithm>
#include <cstring>

#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{

// Set Event Mask: all events enabled.
const std::uint8_t kEventMask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
const std::uint8_t kSimplePairingOn = 0x01;
// Inquiry results that carry the signal strength and the extended inquiry data (names).
const std::uint8_t kInquiryModeExtended = 0x02;
// Class of device 0x000104, least significant byte first: a desktop computer.
const std::uint8_t kClassOfDevice[3] = {0x04, 0x01, 0x00};

} // namespace

BtStack::BtStack(IHciTransport& transport, IBondStore& bonds, IUsbDirectory& usb, IClock& clock)
    : m_transport(transport), m_bondStore(bonds), m_usb(usb), m_clock(clock)
{
}

// ------------------------------------------------------------------
// commands
// ------------------------------------------------------------------
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

bool BtStack::hciSync(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length)
{
    for (int attempt = 1;; attempt++)
    {
        m_ccOp = 0;
        if (!cmd(opcode, params, length))
        {
            return false;
        }
        if (wait([this, opcode] { return m_ccOp == opcode; }, attempt < 4 ? 1500 : 2500, false))
        {
            break;
        }
        if (attempt + 1 == 6)
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

void BtStack::dropStaleLinks()
{
    std::uint8_t scan = 0;
    std::uint8_t disc[3];

    cmd(hci::kWriteScanEnable, &scan, 1);
    poll(50);

    if (m_handle != 0 && !m_disconnected)
    {
        putLe16(disc, m_handle);
        disc[2] = 0x13;
        cmd(hci::kDisconnect, disc, 3);
        wait([this] { return m_disconnected; }, 1500, false);
    }

    // the controller can hold links from a killed payload
    for (int h = 1; h <= 8; h++)
    {
        disc[0] = static_cast<std::uint8_t>(h);
        disc[1] = 0;
        disc[2] = 0x13;
        cmd(hci::kDisconnect, disc, 3);
        poll(40);
    }
    poll(200);

    m_disconnected = false;
    m_connDone = false;
    m_connStatus = 0;
    m_handle = 0;
    logLine("radio: leftover links cleared");
}

// ------------------------------------------------------------------
// the controller
// ------------------------------------------------------------------
void BtStack::useAdapter(const std::string& node)
{
    HciLayout layout;
    const char* which = node.empty() ? m_usb.internalNode() : node.c_str();

    m_node = node;
    m_external = !node.empty();
    m_layoutKnown = false;
    m_layout = HciLayout{};
    // the same lookup for the console's chip and for an adapter: which
    // endpoint is which comes from the descriptors, not from a guess
    if (m_usb.discoverLayout(which, layout))
    {
        logLine("%s %s: Bluetooth interface %u, events %#x, ACL in %#x, ACL "
                "out %#x (from its descriptors)",
                m_external ? "adapter" : "console chip", which, layout.interface, layout.events, layout.aclIn,
                layout.aclOut);
        m_layout = layout;
        m_layoutKnown = true;
    }
    else if (m_external)
    {
        logLine("adapter %s: descriptors not available, assuming the standard "
                "layout (0x81, 0x82, 0x02)",
                which);
        m_layoutKnown = true;
    }
    else if (m_internalStandard)
    {
        m_layoutKnown = true;
    }
    else
    {
        logLine("console chip: descriptors not available, detecting the "
                "layout by behaviour");
        m_layout.events = 0x82; // detected in start()
        m_layout.aclIn = 0x81;
        m_layout.aclOut = 0x01;
    }
    m_transport.select(node.empty() ? nullptr : node.c_str(), m_layout);
}

bool BtStack::start()
{
    std::uint8_t name[248];
    std::uint8_t scan;

    m_startError.clear();
    if (!m_transport.open())
    {
        m_startError =
            m_external ? "the adapter could not be opened" : "the console's Bluetooth chip could not be opened";
        return false;
    }

    // The console's controller exposes the event and ACL pipes on different
    // endpoints depending on the hardware revision; find out where the events
    // come from and reopen with the right roles.
    if (!m_layoutKnown && m_transport.detectRoles(10000) == 2)
    {
        logLine("hci: events arrive on the second IN pipe, "
                "switching to the standard endpoint layout");
        m_transport.close();
        m_layout.events = 0x81;
        m_layout.aclIn = 0x82;
        m_layout.aclOut = 0x02;
        m_transport.select(m_node.empty() ? nullptr : m_node.c_str(), m_layout);
        m_internalStandard = true;
        if (!m_transport.open())
        {
            return false;
        }
    }

    // An adapter that is ours alone starts from a clean state; the console's
    // chip is never reset, the console's stack is using it.
    if (m_external && !hciSync(hci::kReset))
    {
        m_startError = "the adapter did not answer (HCI Reset)";
        m_transport.close();
        return false;
    }

    if (!hciSync(hci::kReadBufferSize))
    {
        m_aclMtu = 0x3fd;
        m_creditsMax = 7;
        m_credits = 7;
        logLine("controller: buffer size reply missed, using 7 x 1021");
    }
    else
    {
        m_credits = getLe16(m_cc + 9);
        m_aclMtu = getLe16(m_cc + 6);
        m_creditsMax = m_credits;
    }
    if (m_aclMtu < 0x40 || m_credits < 1)
    {
        m_aclMtu = 0x3fd;
        m_creditsMax = 7;
        m_credits = 7;
    }
    logLine("controller: %d ACL buffers of %u bytes", m_credits, m_aclMtu);

    // identify the controller and a few of its settings
    if (hciSync(hci::kReadLocalVersion) && m_ccLen >= 14)
    {
        const unsigned hv = m_cc[6], lmp = m_cc[9];
        const unsigned man = getLe16(m_cc + 10);

        logLine("controller: HCI %s (%#x), LMP %#x, manufacturer %#06x (%s), "
                "revision %#x/%#x",
                hciVersionName(hv), hv, lmp, man, manufacturerName(man), getLe16(m_cc + 7), getLe16(m_cc + 12));
    }
    else
    {
        logLine("controller: local version not answered");
    }
    if (hciSync(hci::kReadLocalFeatures) && m_ccLen >= 14)
    {
        // byte 4 bit 5: BR/EDR not supported (a Low Energy only adapter)
        if (m_cc[6 + 4] & 0x20)
        {
            logLine("controller: no BR/EDR support (features %02x %02x %02x "
                    "%02x %02x)",
                    m_cc[6], m_cc[7], m_cc[8], m_cc[9], m_cc[10]);
            if (m_external)
            {
                m_startError = "this adapter has no Bluetooth Classic (BR/EDR); audio needs it";
                m_transport.close();
                return false;
            }
        }
    }
    if (hciSync(hci::kReadBdAddr) && m_ccLen >= 12)
    {
        std::memcpy(m_local.data(), m_cc + 6, 6);
        m_localKnown = true;
        logLine("controller address %s", addressText(m_local).c_str());
    }
    else
    {
        m_local = BdAddr{};
        m_localKnown = true; // pairings go under a zero address
        logLine("controller address unknown");
    }
    if (hciSync(hci::kReadPageTimeout) && m_ccLen >= 8)
    {
        logLine("controller: page timeout %u ms", static_cast<unsigned>(getLe16(m_cc + 6) * 625 / 1000));
    }
    if (hciSync(hci::kReadScanEnable) && m_ccLen >= 7)
    {
        logLine("controller: scan setting at start %u (%s)", m_cc[6],
                m_cc[6] == 0   ? "off"
                : m_cc[6] == 1 ? "inquiry scan"
                : m_cc[6] == 2 ? "page scan"
                               : "inquiry and page scan");
    }

    hciSync(hci::kSetEventMask, kEventMask, 8);
    hciSync(hci::kWriteSimplePairing, &kSimplePairingOn, 1);
    hciSync(hci::kWriteInquiryMode, &kInquiryModeExtended, 1);
    hciSync(hci::kWriteClassOfDevice, kClassOfDevice, 3);

    std::memset(name, 0, sizeof name);
    std::strcpy(reinterpret_cast<char*>(name), "BlueV");
    hciSync(hci::kWriteLocalName, name, 248);

    scan = 0;
    hciSync(hci::kWriteScanEnable, &scan, 1);
    scan = 2; // page scan (so we stay findable)
    dropStaleLinks();
    cmd(hci::kWriteScanEnable, &scan, 1);
    poll(50);
    return true;
}

void BtStack::stop()
{
    std::uint8_t scan = 2;

    dropStaleLinks();
    // dropStaleLinks leaves scanning off; the system needs page scan on so
    // that paired controllers can reconnect to the console
    cmd(hci::kWriteScanEnable, &scan, 1);
    poll(100);
    m_transport.close();
    m_localKnown = false;
}

void BtStack::setScan(int mode)
{
    const std::uint8_t scan = static_cast<std::uint8_t>(mode);

    cmd(hci::kWriteScanEnable, &scan, 1);
    poll(50);
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
bool BtStack::bondExists(const BdAddr& peer)
{
    return m_localKnown && m_bondStore.hasKey(m_local, peer);
}

// make the device's bond the current one (loading it when it exists)
// Makes `peer` the device whose key and name are in use (empty when unknown).
void BtStack::setBond(const BdAddr& peer)
{
    m_key = BondKey{};
    m_bondStore.loadKey(m_local, peer, m_key);
    m_peerName = m_bondStore.loadName(peer);
}

bool BtStack::hasBond(const BdAddr& peer)
{
    return bondExists(peer);
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

    if (m_ifCount == 64)
    { // in-flight ring full: drop oldest
        m_ifCount = 63;
        m_ifHead = (m_ifHead + 1) % 64;
    }
    m_inflight[(m_ifCount + m_ifHead) % 64] = m_clock.nowMs();
    m_ifCount++;
    return true;
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
            m_ifHead = (m_ifHead + 1) % 64;
            m_ifCount--;
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
