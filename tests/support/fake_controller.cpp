// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_controller.hpp"

#include <algorithm>
#include <cstring>

#include "core/bluetooth/hci_defs.hpp"

namespace bluev::testing
{

using namespace bluev::hci;

int FakeController::take(std::deque<Bytes>& q, std::uint8_t* out, int max)
{
    if (q.empty())
    {
        return 0;
    }
    const Bytes b = std::move(q.front());
    q.pop_front();
    const int n = std::min<int>(max, static_cast<int>(b.size()));
    std::memcpy(out, b.data(), n);
    return n;
}

void FakeController::event(std::uint8_t code, const Bytes& params, std::uint32_t delayMs)
{
    Bytes e = {code, static_cast<std::uint8_t>(params.size())};
    e.insert(e.end(), params.begin(), params.end());
    if (delayMs == 0)
    {
        m_events.push_back(e);
    }
    else
    {
        m_scheduled.push_back({m_clock.nowMs() + delayMs, false, e});
    }
}

void FakeController::acl(const Bytes& packet, std::uint32_t delayMs)
{
    if (delayMs == 0)
    {
        m_acl.push_back(packet);
    }
    else
    {
        m_scheduled.push_back({m_clock.nowMs() + delayMs, true, packet});
    }
}

void FakeController::l2cap(std::uint16_t cid, const Bytes& payload, std::uint16_t handle, std::uint32_t delayMs)
{
    if (handle == 0)
    {
        handle = remote.handle;
    }
    const std::uint16_t l2len = static_cast<std::uint16_t>(payload.size());
    const std::uint16_t total = static_cast<std::uint16_t>(l2len + 4);
    Bytes p = {static_cast<std::uint8_t>(handle & 0xff), static_cast<std::uint8_t>((handle >> 8) | 0x20),
               static_cast<std::uint8_t>(total & 0xff),  static_cast<std::uint8_t>(total >> 8),
               static_cast<std::uint8_t>(l2len & 0xff),  static_cast<std::uint8_t>(l2len >> 8),
               static_cast<std::uint8_t>(cid & 0xff),    static_cast<std::uint8_t>(cid >> 8)};
    p.insert(p.end(), payload.begin(), payload.end());
    acl(p, delayMs);
}

void FakeController::completedPackets(std::uint16_t handle, std::uint16_t count, std::uint32_t delayMs)
{
    event(0x13,
          {1, static_cast<std::uint8_t>(handle & 0xff), static_cast<std::uint8_t>(handle >> 8),
           static_cast<std::uint8_t>(count & 0xff), static_cast<std::uint8_t>(count >> 8)},
          delayMs);
}

void FakeController::remoteDisconnect(std::uint8_t reason, std::uint32_t delayMs)
{
    event(0x05,
          {0, static_cast<std::uint8_t>(remote.handle & 0xff), static_cast<std::uint8_t>(remote.handle >> 8), reason},
          delayMs);
    m_linkUp = false;
}

void FakeController::complete(std::uint16_t opcode, const Bytes& ret, std::uint32_t delayMs)
{
    Bytes p = {1, static_cast<std::uint8_t>(opcode & 0xff), static_cast<std::uint8_t>(opcode >> 8)};
    p.insert(p.end(), ret.begin(), ret.end());
    event(0x0e, p, delayMs);
}

void FakeController::status(std::uint16_t opcode, std::uint8_t code, std::uint32_t delayMs)
{
    event(0x0f, {code, 1, static_cast<std::uint8_t>(opcode & 0xff), static_cast<std::uint8_t>(opcode >> 8)}, delayMs);
}

void FakeController::connectionComplete(std::uint8_t statusCode, std::uint32_t delayMs)
{
    Bytes p = {statusCode, static_cast<std::uint8_t>(remote.handle & 0xff),
               static_cast<std::uint8_t>(remote.handle >> 8)};
    p.insert(p.end(), remote.address.begin(), remote.address.end());
    p.push_back(1); // link type: ACL
    p.push_back(0); // encryption off
    event(0x03, p, delayMs);
    m_linkUp = statusCode == 0;
}

void FakeController::authenticated(std::uint32_t delayMs)
{
    const std::uint8_t st = remote.authFails ? 0x06 : 0;
    const std::uint8_t lo = remote.handle & 0xff, hi = remote.handle >> 8;

    event(0x06, {st, lo, hi}, delayMs);
    if (!remote.authFails && remote.encrypts)
    {
        event(0x08, {0, lo, hi, 1}, delayMs + 50);
    }
}

bool FakeController::command(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length)
{
    SentCommand c{opcode, Bytes(params, params + length)};

    commands.push_back(c);
    if (std::find(silentOpcodes.begin(), silentOpcodes.end(), opcode) != silentOpcodes.end())
    {
        return true;
    }
    if (std::find(failingOpcodes.begin(), failingOpcodes.end(), opcode) != failingOpcodes.end())
    {
        complete(opcode, {0x01});
        return true;
    }
    respond(opcode, c.params);
    return true;
}

void FakeController::respond(std::uint16_t opcode, const Bytes& p)
{
    const std::uint8_t lo = remote.handle & 0xff, hi = remote.handle >> 8;

    switch (opcode)
    {
    case kReadBufferSize:
        complete(opcode,
                 {0, static_cast<std::uint8_t>(aclMtu & 0xff), static_cast<std::uint8_t>(aclMtu >> 8), 0xff,
                  static_cast<std::uint8_t>(aclBuffers & 0xff), static_cast<std::uint8_t>(aclBuffers >> 8), 0, 0});
        break;
    case kReadLocalVersion:
        complete(opcode,
                 {0, hciVersion, static_cast<std::uint8_t>(hciRevision & 0xff),
                  static_cast<std::uint8_t>(hciRevision >> 8), hciVersion,
                  static_cast<std::uint8_t>(manufacturer & 0xff), static_cast<std::uint8_t>(manufacturer >> 8),
                  static_cast<std::uint8_t>(lmpSubversion & 0xff), static_cast<std::uint8_t>(lmpSubversion >> 8)});
        break;
    case 0xfc6d: // Realtek: read ROM version
        complete(opcode, {0, romVersion});
        break;
    case 0xfc61: // Realtek: read register 16 (the security key id)
        complete(opcode, {0, keyId, 0});
        break;
    case 0xfc66: // Realtek: drop the vendor patch, then report the ROM identity
        if (dropToRomOnFc66)
        {
            manufacturer = 0x005d;
            lmpSubversion = 0x8761;
            hciRevision = 0x000b;
            hciVersion = 0x0a;
        }
        break;   // no answer: the stack does not wait for one
    case 0xfc20: // Realtek: one firmware fragment
        complete(opcode, {0, static_cast<std::uint8_t>(p.empty() ? 0 : p[0])});
        break;
    case kReadLocalFeatures:
        complete(opcode, {0, 0xff, 0xff, 0xcd, 0xfe, static_cast<std::uint8_t>(bredr ? 0x98 : 0xb8), 0x59, 0x83, 0x7a});
        break;
    case kReadBdAddr: {
        Bytes r = {0};
        r.insert(r.end(), localAddress.begin(), localAddress.end());
        complete(opcode, r);
        break;
    }
    case kReadPageTimeout:
        complete(opcode, {0, 0x00, 0x20});
        break;
    case kReadScanEnable:
        complete(opcode, {0, scanSetting});
        break;
    case kInquiry: {
        status(opcode, 0);
        std::uint32_t at = 200;
        for (const InquiryDevice& d : inquiryDevices)
        {
            Bytes r = {1};
            r.insert(r.end(), d.address.begin(), d.address.end());
            r.push_back(d.pageScanRepMode);
            r.push_back(0);
            r.push_back(static_cast<std::uint8_t>(d.classOfDevice & 0xff));
            r.push_back(static_cast<std::uint8_t>((d.classOfDevice >> 8) & 0xff));
            r.push_back(static_cast<std::uint8_t>(d.classOfDevice >> 16));
            r.push_back(static_cast<std::uint8_t>(d.clockOffset & 0xff));
            r.push_back(static_cast<std::uint8_t>(d.clockOffset >> 8));
            r.push_back(static_cast<std::uint8_t>(d.rssi));
            if (!d.name.empty())
            {
                r.push_back(static_cast<std::uint8_t>(d.name.size() + 1));
                r.push_back(0x09);
                r.insert(r.end(), d.name.begin(), d.name.end());
            }
            event(0x2f, r, at);
            at += 100;
        }
        event(0x01, {0}, inquiryDurationMs);
        break;
    }
    case kInquiryCancel:
        complete(opcode, {0});
        m_scheduled.erase(std::remove_if(m_scheduled.begin(), m_scheduled.end(),
                                         [](const Scheduled& s) { return !s.isAcl && s.data[0] == 0x01; }),
                          m_scheduled.end());
        break;
    case kCreateConn: {
        status(opcode, 0);
        if (remote.page == Remote::Page::Accept)
        {
            connectionComplete(0, remote.pageDelayMs);
        }
        else if (remote.page == Remote::Page::Fail)
        {
            connectionComplete(remote.failStatus, remote.pageDelayMs);
        }
        else if (remote.page == Remote::Page::AlreadyConnected)
        {
            connectionComplete(0x0b, remote.pageDelayMs);
        }
        break;
    }
    case kCreateConnCancel:
        complete(opcode, {0xff});
        break;
    case kAcceptConnReq:
        status(opcode, 0);
        connectionComplete(0, 200);
        break;
    case kDisconnect:
        status(opcode, 0);
        if (m_linkUp && p.size() >= 2 && p[0] == lo && p[1] == hi)
        {
            event(0x05, {0, lo, hi, p.size() > 2 ? p[2] : static_cast<std::uint8_t>(0x16)}, 100);
            m_linkUp = false;
        }
        break;
    case kAuthRequested:
        status(opcode, 0);
        event(0x17, Bytes(remote.address.begin(), remote.address.end()), 100); // link key request
        break;
    case kLinkKeyReply:
        complete(opcode, Bytes{0});
        authenticated(100);
        break;
    case kLinkKeyNegReply:
        complete(opcode, Bytes{0});
        event(0x31, Bytes(remote.address.begin(), remote.address.end()), 100);
        break;
    case kIoCapabilityReply:
        complete(opcode, Bytes{0});
        event(0x33, Bytes(remote.address.begin(), remote.address.end()), 100); // user confirmation request
        break;
    case kUserConfirmReply: {
        complete(opcode, Bytes{0});
        Bytes r(remote.address.begin(), remote.address.end());
        r.insert(r.end(), 16, 0xa5); // the new link key
        r.push_back(4);              // authenticated combination key
        event(0x18, r, 100);
        authenticated(150);
        break;
    }
    case kSetConnEncryption:
        status(opcode, 0);
        event(0x08, {0, lo, hi, 1}, 100);
        break;
    case kRoleDiscovery:
        complete(opcode, {0, lo, hi, 0});
        break;
    default:
        complete(opcode, {0});
        break;
    }
}

bool FakeController::pump(std::uint32_t timeoutMs)
{
    if (!m_events.empty() || !m_acl.empty())
    {
        return true;
    }

    const std::uint64_t until = m_clock.nowMs() + timeoutMs;
    for (;;)
    {
        // the earliest scheduled packet not later than the timeout
        auto next = m_scheduled.end();
        for (auto it = m_scheduled.begin(); it != m_scheduled.end(); ++it)
        {
            if (it->at <= until && (next == m_scheduled.end() || it->at < next->at))
            {
                next = it;
            }
        }
        if (next == m_scheduled.end())
        {
            m_clock.set(until);
            return false;
        }
        if (next->at > m_clock.nowMs())
        {
            m_clock.set(next->at);
        }
        // everything that is due by now arrives together, in time order
        std::stable_sort(m_scheduled.begin(), m_scheduled.end(),
                         [](const Scheduled& a, const Scheduled& b) { return a.at < b.at; });
        for (auto it = m_scheduled.begin(); it != m_scheduled.end();)
        {
            if (it->at <= m_clock.nowMs())
            {
                (it->isAcl ? m_acl : m_events).push_back(it->data);
                it = m_scheduled.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return true;
    }
}

bool FakeController::sendAcl(const std::uint8_t* packet, std::uint32_t length)
{
    if (!aclSendOk)
    {
        return false;
    }
    aclOut.emplace_back(packet, packet + length);
    if (onAclOut)
    {
        onAclOut(aclOut.back());
    }
    return true;
}

} // namespace bluev::testing
