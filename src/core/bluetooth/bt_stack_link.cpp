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

// The console's stack shares the chip and its reads take some of the pairing
// events; a headset that gets no answer gives up. The headset also starts its
// own authentication seconds after the link is up (once it has looked at our
// SDP records), so the request is repeated now and then and the answer waited
// out; a collision means "already pairing", not a failure.
constexpr std::uint32_t kAuthMs = 18000;
constexpr std::uint32_t kAuthResendMs = 5000;
constexpr std::uint32_t kEncryptionMs = 12000;
constexpr std::uint32_t kEncryptionResendMs = 3000;

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

// Pages the device until the controller answers. The console's page scan takes
// almost all the radio time on this chip, so pageDevice() turns scanning off
// for the whole page; the controller needs about a second to find the device
// then. True when a connection event came (successful or not: m_connStatus
// tells).
bool BtStack::pageAttempts(std::uint8_t* req)
{
    for (int attempt = 1; attempt <= 3; attempt++)
    {
        setPageVariant(req, attempt);
        clearConnectResult();
        if (!cmd(hci::kCreateConn, req, kCreateConnLength))
        {
            return false;
        }
        // the controller's page timeout is 15 s, so a device that does not
        // answer costs one timeout here, not four
        const bool answered = wait([this] { return m_connDone; }, 12000);
        if (answered)
        {
            return true;
        }
        logLine("connection: no answer (try %d)", attempt);
        cmd(hci::kCreateConnCancel, m_target.data(), 6);
        poll(400);
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

    // The console's page scan runs at nearly full duty on its chip and leaves a
    // page almost no radio time, so it is off for the whole page and put back
    // when the page is over (or failed). Adapters are left as they were.
    const bool pauseScan = !m_external;
    if (pauseScan)
    {
        setScan(0);
    }
    bool linked = false;
    if (pageAttempts(req))
    {
        if (m_connStatus == 0)
        {
            linked = true;
        }
        else if (m_connStatus == 0x0b)
        {
            linked = recoverExistingLink(req);
        }
        else
        {
            logLine("connection failed: %s (status %#04x)%s", hciStatusName(m_connStatus), m_connStatus,
                    m_connStatus == 4 ? " (the device may be off, out of range or connected to something else)" : "");
        }
    }
    if (pauseScan)
    {
        restoreScan();
    }
    return linked;
}

// Authenticates the link; pairing happens here when there is no key. False when
// it fails, and a key the device no longer accepts is deleted.
bool BtStack::authenticateLink()
{
    std::uint8_t req[3];

    putLe16(req, m_handle);
    // The headset takes up to about ten seconds to start pairing by itself; our
    // request before that is answered with "no answer" or "LMP error transaction
    // collision", and the collision means a transaction is already running. So
    // the request is only repeated as a nudge and the answer is waited out.
    m_authDone = false;
    m_authStatus = 0;
    bool collisionLogged = false;
    bool sent = false;
    std::uint64_t lastSend = m_clock.nowMs();
    const std::uint64_t deadline = m_clock.nowMs() + kAuthMs;

    while (!m_authDone && !m_disconnected && m_clock.nowMs() < deadline)
    {
        if (!sent || m_clock.nowMs() - lastSend >= kAuthResendMs)
        {
            if (!cmd(hci::kAuthRequested, req, 2))
            {
                return false;
            }
            sent = true;
            lastSend = m_clock.nowMs();
        }
        poll(500);
        if (m_authDone && (m_authStatus == 0x23 || m_authStatus == 0x0c))
        {
            if (!collisionLogged)
            {
                logLine("authentication: the headset is already pairing");
                collisionLogged = true;
            }
            m_authDone = false; // still in progress, not a failure
        }
    }
    if (m_disconnected)
    {
        logLine("authentication: the link went away");
        return false;
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
    return true;
}

// Authenticates the link and turns encryption on. False when either fails.
bool BtStack::encryptLink()
{
    std::uint8_t req[3];

    // A device we have a key for does not encrypt the link by itself within a
    // moment (the WH-CH720N and the Redmi Buds never did, in 2 s of waiting),
    // so it is asked soon. A new pairing is left the longer time: the controller
    // starts the encryption itself when the pairing is done.
    const std::uint32_t encryptionGraceMs = m_key.have ? 500 : 4000;

    if (!authenticateLink())
    {
        return false;
    }

    // wait for the link to encrypt itself; force it if needed, and keep asking
    // while it does not. A fresh pairing takes a moment to settle in the headset,
    // which can refuse the command while it is still busy with it.
    wait([this] { return m_encOn || m_disconnected; }, encryptionGraceMs, false);
    const std::uint64_t encryptionDeadline = m_clock.nowMs() + kEncryptionMs;
    bool encryptionSent = false;
    bool encryptionStatusLogged = false;
    std::uint64_t lastEncryption = m_clock.nowMs();

    while (!m_encOn && !m_disconnected && m_clock.nowMs() < encryptionDeadline)
    {
        if (!encryptionSent || m_clock.nowMs() - lastEncryption >= kEncryptionResendMs)
        {
            putLe16(req, m_handle);
            req[2] = 1;
            if (!cmd(hci::kSetConnEncryption, req, 3))
            {
                return false;
            }
            encryptionSent = true;
            lastEncryption = m_clock.nowMs();
            m_encCmdDone = false;
        }
        poll(500);
        if (m_encCmdDone && m_encCmdStatus != 0 && !encryptionStatusLogged)
        {
            logLine("encryption: refused (%s, status %#04x)", hciStatusName(static_cast<unsigned>(m_encCmdStatus)),
                    m_encCmdStatus);
            encryptionStatusLogged = true;
        }
    }
    if (!m_encOn)
    {
        if (m_disconnected)
        {
            logLine("encryption: the link went away");
            return false;
        }
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
    const bool hadKey = m_key.have;
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
    const std::uint64_t began = m_clock.nowMs();

    if (!m_external)
    {
        setSimplePairing();
    }
    if (!pageDevice(peer, psrm, clock))
    {
        logLine("connect: no link after %.1f s", static_cast<double>(m_clock.nowMs() - began) / 1000.0);
        return false;
    }
    const std::uint64_t paged = m_clock.nowMs();
    bool secured;

    if (!hadKey && !m_external)
    {
        // The headset does not encrypt the link it was just paired on, and does
        // not answer AVDTP on it either (Discover and configure stay unanswered
        // until it times out); the next link does both. So the link is dropped as
        // soon as the key is saved and dialled again.
        secured = authenticateLink();
        if (secured)
        {
            logLine("connect: paired, dialling again for the audio link");
            disconnect();
            // the headset still has its A2DP state on the dropped link for a
            // moment and refuses the transport channel until it has cleared it
            poll(2500);
            secured = (linkUp() || pageDevice(peer, psrm, clock)) && encryptLink();
        }
    }
    else
    {
        secured = encryptLink();
    }

    logLine("connect: paging %.1f s, security %.1f s%s", static_cast<double>(paged - began) / 1000.0,
            static_cast<double>(m_clock.nowMs() - paged) / 1000.0, secured ? "" : " (failed)");
    return secured;
}

// a paired device dialed in while idle; authenticate and encrypt
bool BtStack::adoptIncoming()
{
    m_peerMac = addressString(m_target);
    logLine("%s connected by itself", m_peerName.empty() ? m_peerMac.c_str() : m_peerName.c_str());
    const std::uint64_t began = m_clock.nowMs();
    const bool secured = encryptLink();

    logLine("connect: security %.1f s%s", static_cast<double>(m_clock.nowMs() - began) / 1000.0,
            secured ? "" : " (failed)");
    return secured;
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
        restoreScan();
        return;
    }
    resetLinkState();
    restoreScan();
}

} // namespace bluev
