// SPDX-License-Identifier: GPL-3.0-or-later
// paging, encryption and the connect / disconnect entry points
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

// Polls until the connection complete event of the target has come (the result
// is in m_connStatus); false on timeout or when the link is lost first.
bool BtStack::waitConn(std::uint32_t timeoutMs)
{
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;

    while (!m_connDone)
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
        if (m_disconnected)
        {
            return false;
        }
    }
    return true;
}

// Creates the ACL connection to `peer`, trying up to four times and then
// listening for a while, because a headset may be paging us at the same time.
// `psrm` and `clock` are what the inquiry reported (the page scan repetition
// mode and the clock offset). On success the link is up, m_handle is set.
bool BtStack::pageDevice(const BdAddr& peer, std::uint8_t psrm, std::uint16_t clock)
{
    std::uint8_t req[13];
    std::uint8_t disc[3];
    bool gotIt = false;

    m_target = peer;
    m_targetPsrm = static_cast<std::uint8_t>(psrm + (psrm == 0)); // R0 (0) is paged as R1
    m_targetClock = clock;

    m_connDone = false;
    m_connStatus = 0;

    std::memcpy(req, peer.data(), 6);
    req[6] = 0x18;
    req[7] = 0xcc; // packet types DM1..DH5
    req[8] = m_targetPsrm;
    req[9] = 0; // reserved
    req[10] = clock & 0xff;
    req[11] = static_cast<std::uint8_t>(clock >> 8);
    req[12] = 1; // allow role switch

    for (int attempt = 1; attempt <= 4 && !gotIt; attempt++)
    {
        // The console's page scan for the gamepad takes almost all the radio
        // time; pause it for our page, then put it back before listening for
        // the headset's own call.
        pauseSystemScan();
        // alternate the packet types / role switch between the tries: the odd
        // tries use DM1 only and no role switch, the even ones every packet type
        if (attempt % 2 == 0)
        { // every packet type, role switch allowed
            req[6] = 0x18;
            req[7] = 0xcc;
            req[12] = 1;
        }
        else
        { // DM1 only, no role switch
            req[6] = 0x08;
            req[7] = 0x00;
            req[12] = 0;
        }
        if (!cmd(hci::kCreateConn, req, 13))
        {
            restoreSystemScan();
            return false;
        }
        // The page timeout of the controller can be much longer than the 4 s
        // the Slim and the adapters need. On the console's own chip the page
        // was being cancelled before the controller had finished looking, so
        // a headset without a stale bond (no dial-in) could never pair. Give
        // the internal controller its full page timeout.
        const std::uint32_t pageWait = m_external ? 4000u : (m_pageTimeoutMs > 0 ? m_pageTimeoutMs + 1000 : 16000u);
        if (waitConn(pageWait))
        {
            if (m_connStatus == 0)
            {
                gotIt = true;
                break;
            }
            logLine("connection: page failed: %s (status %#04x)", hciStatusName(m_connStatus), m_connStatus);
            if (m_external)
            {
                // a completed failure is final for an adapter, as before
                gotIt = true;
                break;
            }
            m_connDone = false;
            m_connStatus = 0;
        }
        else
        {
            logLine("connection: no answer (try %d)", attempt);
            cmd(hci::kCreateConnCancel, m_target.data(), 6);
            poll(400);
            m_connDone = false;
            m_connStatus = 0;
        }
        // stop paging for a moment: the headset may be paging us. The system
        // page scan has to be back on for that.
        restoreSystemScan();
        if (waitConn(20000) && m_connStatus == 0)
        {
            gotIt = true;
            break;
        }
        m_connDone = false;
        m_connStatus = 0;
    }
    restoreSystemScan();
    if (!gotIt)
    {
        return false;
    }

    if (m_connStatus == 0)
    {
        return true;
    }
    if (m_connStatus == 0x0b)
    {
        // The controller says an ACL link to this device exists. It may be ours,
        // completing late: wait for its event. If not, drop every leftover link
        // and page again.
        m_connDone = false;
        m_connStatus = 0;
        if (waitConn(4000) && m_connStatus == 0)
        {
            logLine("connection: the earlier attempt completed");
            return true;
        }
        logLine("ACL already exists, dropping leftover links on this controller");
        cmd(hci::kCreateConnCancel, m_target.data(), 6);
        for (int h = 1; h <= 8; h++)
        {
            disc[0] = static_cast<std::uint8_t>(h);
            disc[1] = 0;
            disc[2] = 0x13;
            cmd(hci::kDisconnect, disc, 3);
            poll(120);
        }
        poll(400);
        m_connDone = false;
        m_connStatus = 0;
        m_handle = 0;
        m_disconnected = false;
        if (!cmd(hci::kCreateConn, req, 13))
        {
            return false;
        }
        if (waitConn(8000) && m_connStatus == 0)
        {
            return true;
        }
        logLine("connection: an earlier link to this device is still there: "
                "%s (status %#04x)",
                hciStatusName(m_connStatus), m_connStatus);
        notify("BlueV: turn the device off and on, then try again");
        return false;
    }
    const char* why = "";

    if (m_connStatus == 4)
    {
        why = " (the device may be off, out of range or connected to something else)";
    }
    logLine("connection failed: %s (status %#04x)%s", hciStatusName(m_connStatus), m_connStatus, why);
    return false;
}

// Authenticates (pairing happens here when there is no key) and turns encryption
// on. False when either fails; a key the device no longer accepts is deleted.
bool BtStack::encryptLink()
{
    std::uint8_t req[3];

    req[0] = m_handle & 0xff;
    req[1] = static_cast<std::uint8_t>(m_handle >> 8);
    m_authDone = false;
    if (!cmd(hci::kAuthRequested, req, 2))
    {
        return false;
    }

    std::uint64_t deadline = m_clock.nowMs() + 30000;
    while (!m_authDone)
    {
        std::int64_t left = static_cast<std::int64_t>(deadline - m_clock.nowMs());

        if (left < 1)
        {
            break;
        }
        if (left > 100)
        {
            left = 100;
        }
        poll(static_cast<std::uint32_t>(left));
        if (m_disconnected)
        {
            break;
        }
    }
    if (!m_authDone)
    {
        logLine("authentication: no answer");
        return false;
    }
    if (m_authStatus != 0)
    {
        logLine("authentication failed: %s (status %#04x)", hciStatusName(m_authStatus), m_authStatus);
        if (!m_key.have)
        {
            return false;
        }
        // a stale stored key: forget it and pair again
        logLine("forgetting the stored key; pair the device again");
        m_bondStore.removeKey(m_local, m_key.peer);
        m_key = BondKey{};
        return false;
    }

    // wait for the link to encrypt itself; force it if needed
    deadline = m_clock.nowMs() + 2000;
    while (!m_encOn && !m_disconnected)
    {
        std::int64_t left = static_cast<std::int64_t>(deadline - m_clock.nowMs());

        if (left < 1)
        {
            break;
        }
        if (left > 100)
        {
            left = 100;
        }
        poll(static_cast<std::uint32_t>(left));
    }
    if (!m_encOn && !m_disconnected)
    {
        req[0] = m_handle & 0xff;
        req[1] = static_cast<std::uint8_t>(m_handle >> 8);
        req[2] = 1;
        if (!cmd(hci::kSetConnEncryption, req, 3))
        {
            return false;
        }
        deadline = m_clock.nowMs() + 20000;
        while (!m_encOn && !m_disconnected)
        {
            if (m_clock.nowMs() >= deadline)
            {
                break;
            }
            poll(100);
        }
    }
    if (!m_encOn)
    {
        logLine("encryption failed: %s (status %#04x)", hciStatusName(m_encStatus), m_encStatus);
        return false;
    }
    logLine("link encrypted");
    const std::uint8_t handle[2] = {static_cast<std::uint8_t>(m_handle & 0xff),
                                    static_cast<std::uint8_t>(m_handle >> 8)};

    cmd(hci::kRoleDiscovery, handle, 2);
    poll(100);
    return true;
}

void BtStack::resetLinkState()
{
    m_connDone = false;
    m_connStatus = 0;
    m_handle = 0;
    m_authDone = false;
    m_authStatus = 0;
    m_encOn = false;
    m_encStatus = 0;
    m_disconnected = false;
    m_reassembler.reset();
    m_credits = m_creditsMax;
    m_ifHead = 0;
    m_ifCount = 0;
}

// forget the link state and the target
void BtStack::linkReset()
{
    resetLinkState();
    m_target = BdAddr{};
}

// connect to (and pair with, when no bond exists) a chosen device
bool BtStack::connectTo(const BdAddr& peer)
{
    const BtDevice* d = nullptr;
    std::uint8_t psrm = 1;
    std::uint16_t clock = 0;

    for (const BtDevice& candidate : m_devices)
    {
        if (candidate.address == peer)
        {
            d = &candidate;
        }
    }

    resetLinkState();
    m_target = peer;
    setBond(peer);
    if (d != nullptr)
    {
        psrm = d->pageScanRepMode ? d->pageScanRepMode : 1;
        clock = d->clockOffset;
        if (!d->name.empty())
        {
            m_peerName = d->name.substr(0, 63);
            m_bondStore.saveName(peer, m_peerName);
        }
    }
    m_peerMac = addressString(peer);
    logLine("connecting to %s %s%s", addressText(peer).c_str(), m_peerName.c_str(),
            m_key.have ? " (paired before)" : " (new device)");
    if (!pageDevice(peer, psrm, clock))
    {
        restoreSystemScan();
        return false;
    }
    restoreSystemScan();
    return encryptLink();
}

// a paired device dialed in while idle; authenticate and encrypt
bool BtStack::adoptIncoming()
{
    m_peerMac = addressString(m_target);
    logLine("%s connected by itself", m_peerName.empty() ? m_peerMac.c_str() : m_peerName.c_str());
    return encryptLink();
}

void BtStack::disconnect()
{
    std::uint8_t disc[3];
    const unsigned oldGen = m_connGen;

    if (m_handle != 0 && !m_disconnected)
    {
        disc[0] = m_handle & 0xff;
        disc[1] = static_cast<std::uint8_t>(m_handle >> 8);
        disc[2] = 0x13;
        cmd(hci::kDisconnect, disc, 3);
        const std::uint64_t deadline = m_clock.nowMs() + 2000;
        while (!m_disconnected && m_clock.nowMs() < deadline)
        {
            poll(100);
        }
    }
    cmd(hci::kCreateConnCancel, m_target.data(), 6);
    poll(200);
    if (m_connGen != oldGen && m_connDone && m_connStatus == 0 && m_handle != 0 && !m_disconnected)
    {
        // the device dialed in again while we were tearing down: that link is
        // the next session, keep it
        setScan(2);
        return;
    }
    resetLinkState();
    setScan(2);
}

} // namespace bluev
