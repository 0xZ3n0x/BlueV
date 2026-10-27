// SPDX-License-Identifier: GPL-3.0-or-later
// paging, encryption and the connect / disconnect entry points
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

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
    putLe16(req + 10, clock);
    req[12] = 1; // allow role switch

    for (int attempt = 1; attempt <= 4 && !gotIt; attempt++)
    {
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
            return false;
        }
        if (wait([this] { return m_connDone; }, 4000))
        {
            gotIt = true;
            break;
        }
        logLine("connection: no answer (try %d)", attempt);
        cmd(hci::kCreateConnCancel, m_target.data(), 6);
        poll(400);
        m_connDone = false;
        m_connStatus = 0;
        // stop paging for a moment: the headset may be paging us
        if (wait([this] { return m_connDone; }, 20000) && m_connStatus == 0)
        {
            gotIt = true;
            break;
        }
        m_connDone = false;
        m_connStatus = 0;
    }
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
        if (wait([this] { return m_connDone; }, 4000) && m_connStatus == 0)
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
        if (wait([this] { return m_connDone; }, 8000) && m_connStatus == 0)
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

    putLe16(req, m_handle);
    m_authDone = false;
    if (!cmd(hci::kAuthRequested, req, 2))
    {
        return false;
    }

    wait([this] { return m_authDone; }, 30000);
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
    wait([this] { return m_encOn || m_disconnected; }, 2000, false);
    if (!m_encOn && !m_disconnected)
    {
        putLe16(req, m_handle);
        req[2] = 1;
        if (!cmd(hci::kSetConnEncryption, req, 3))
        {
            return false;
        }
        wait([this] { return m_encOn || m_disconnected; }, 20000, false);
    }
    if (!m_encOn)
    {
        logLine("encryption failed: %s (status %#04x)", hciStatusName(m_encStatus), m_encStatus);
        return false;
    }
    logLine("link encrypted");
    std::uint8_t handle[2];

    putLe16(handle, m_handle);

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
        return false;
    }
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
        putLe16(disc, m_handle);
        disc[2] = 0x13;
        cmd(hci::kDisconnect, disc, 3);
        wait([this] { return m_disconnected; }, 2000, false);
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
