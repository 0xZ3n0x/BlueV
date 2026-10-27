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

// The Create Connection parameters: the device, the packet types, the page scan
// repetition mode and clock offset the inquiry reported, and whether a role
// switch is allowed. Fixed offsets, shared by every try.
namespace
{
constexpr int kCreateConnLength = 13;
using CreateConn = std::uint8_t[kCreateConnLength];

// Alternates between the tries: the odd ones use DM1 only and no role switch,
// the even ones every packet type.
void setPageVariant(CreateConn req, int attempt)
{
    const bool everything = attempt % 2 == 0;

    req[6] = everything ? 0x18 : 0x08;
    req[7] = everything ? 0xcc : 0x00;
    req[12] = everything ? 1 : 0;
}
} // namespace

// Pages the device up to four times, then listens for a while, because a
// headset may be paging us at the same time. True when a connection event
// came (successful or not: m_connStatus tells).
bool BtStack::pageAttempts(std::uint8_t* req)
{
    for (int attempt = 1; attempt <= 4; attempt++)
    {
        setPageVariant(req, attempt);
        if (!cmd(hci::kCreateConn, req, kCreateConnLength))
        {
            return false;
        }
        if (wait([this] { return m_connDone; }, 4000))
        {
            return true;
        }
        logLine("connection: no answer (try %d)", attempt);
        cmd(hci::kCreateConnCancel, m_target.data(), 6);
        poll(400);
        clearConnectResult();
        // stop paging for a moment: the headset may be paging us
        if (wait([this] { return m_connDone; }, 20000) && m_connStatus == 0)
        {
            return true;
        }
        clearConnectResult();
    }
    return false;
}

// The controller says an ACL link to this device exists. It may be ours,
// completing late: wait for its event. If not, drop every leftover link and
// page again.
bool BtStack::recoverExistingLink(std::uint8_t* req)
{
    clearConnectResult();
    if (wait([this] { return m_connDone; }, 4000) && m_connStatus == 0)
    {
        logLine("connection: the earlier attempt completed");
        return true;
    }
    logLine("ACL already exists, dropping leftover links on this controller");
    cmd(hci::kCreateConnCancel, m_target.data(), 6);
    dropAllLinks(120);
    poll(400);
    clearConnectResult();
    m_handle = 0;
    m_disconnected = false;
    if (!cmd(hci::kCreateConn, req, kCreateConnLength))
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

// Creates the ACL connection to `peer`. `psrm` and `clock` are what the
// inquiry reported (the page scan repetition mode and the clock offset). On
// success the link is up, m_handle is set.
bool BtStack::pageDevice(const BdAddr& peer, std::uint8_t psrm, std::uint16_t clock)
{
    CreateConn req;

    m_target = peer;
    clearConnectResult();

    std::memcpy(req, peer.data(), 6);
    req[8] = static_cast<std::uint8_t>(psrm + (psrm == 0)); // R0 (0) is paged as R1
    req[9] = 0;                                             // reserved
    putLe16(req + 10, clock);

    if (!pageAttempts(req))
    {
        return false;
    }
    if (m_connStatus == 0)
    {
        return true;
    }
    if (m_connStatus == 0x0b)
    {
        return recoverExistingLink(req);
    }
    logLine("connection failed: %s (status %#04x)%s", hciStatusName(m_connStatus), m_connStatus,
            m_connStatus == 4 ? " (the device may be off, out of range or connected to something else)" : "");
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
    clearConnectResult();
    m_handle = 0;
    clearSecurity();
    m_disconnected = false;
    m_reassembler.reset();
    resetCredits();
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
    const unsigned oldGen = m_connGen;

    if (m_handle != 0 && !m_disconnected)
    {
        sendDisconnect(m_handle);
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
