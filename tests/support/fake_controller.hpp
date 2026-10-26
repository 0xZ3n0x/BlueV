// SPDX-License-Identifier: GPL-3.0-or-later
// A Bluetooth controller behind the HCI transport port. It answers the
// commands the stack sends, plays a remote device when asked to (paging,
// pairing, encryption, inquiry), and keeps everything the stack sent. Time
// passes in pump(): the clock jumps to the next scheduled event.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/bluetooth/address.hpp"
#include "core/ports/hci_transport.hpp"
#include "fake_clock.hpp"

namespace bluev::testing
{

using Bytes = std::vector<std::uint8_t>;

struct SentCommand
{
    std::uint16_t opcode;
    Bytes params;
};

struct InquiryDevice
{
    BdAddr address{};
    std::uint32_t classOfDevice = 0x240404; // headset
    std::string name;
    std::int8_t rssi = -50;
    std::uint8_t pageScanRepMode = 1;
    std::uint16_t clockOffset = 0x1234;
};

class FakeController final : public IHciTransport
{
  public:
    explicit FakeController(FakeClock& clock) : m_clock(clock)
    {
    }

    // ---- IHciTransport ----
    void select(const char* node, const HciLayout& layout) override
    {
        selectedNode = node ? node : "";
        selectedLayout = layout;
        selectCount++;
    }
    bool open() override
    {
        opens++;
        return openOk;
    }
    void close() override
    {
        closes++;
    }
    bool command(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length) override;
    bool pump(std::uint32_t timeoutMs) override;
    int nextEvent(std::uint8_t* out, int max) override
    {
        return take(m_events, out, max);
    }
    int nextAcl(std::uint8_t* out, int max) override
    {
        return take(m_acl, out, max);
    }
    bool sendAcl(const std::uint8_t* packet, std::uint32_t length) override;
    int detectRoles(std::uint32_t) override
    {
        return detectResult;
    }
    std::string describe(const char* why) override
    {
        return std::string("fake transport: ") + why;
    }

    // ---- what the test sets up ----
    BdAddr localAddress{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
    std::uint16_t aclMtu = 1021;
    std::uint16_t aclBuffers = 8;
    std::uint8_t hciVersion = 6;
    std::uint16_t manufacturer = 0x000a; // CSR
    bool bredr = true;
    bool openOk = true;
    bool aclSendOk = true; // sendAcl() fails when off
    int detectResult = 1;
    std::vector<std::uint16_t> silentOpcodes;  // commands that get no answer
    std::vector<std::uint16_t> failingOpcodes; // commands answered with status 0x01

    // the remote device
    struct Remote
    {
        BdAddr address{{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}};
        enum class Page
        {
            Accept,
            Silent,
            Fail,
            AlreadyConnected
        } page = Page::Accept;
        std::uint8_t failStatus = 0x04;
        std::uint32_t pageDelayMs = 300;
        bool authFails = false; // authentication status 0x06 (a stale key)
        bool encrypts = true;   // encryption change after authentication
        std::uint16_t handle = 0x0042;
    } remote;
    std::vector<InquiryDevice> inquiryDevices;
    std::uint32_t inquiryDurationMs = 3000;
    int inquiryRefusals = 0; // answer this many Inquiry commands with 0x0c first

    // ---- what the test reads ----
    std::vector<SentCommand> commands;
    std::vector<Bytes> aclOut; // whole ACL packets the stack sent
    std::string selectedNode;
    HciLayout selectedLayout;
    int selectCount = 0, opens = 0, closes = 0;

    bool sawCommand(std::uint16_t opcode) const
    {
        return countCommand(opcode) > 0;
    }
    int countCommand(std::uint16_t opcode) const
    {
        int n = 0;
        for (const SentCommand& c : commands)
        {
            n += c.opcode == opcode;
        }
        return n;
    }
    const SentCommand* lastCommand(std::uint16_t opcode) const
    {
        for (auto it = commands.rbegin(); it != commands.rend(); ++it)
        {
            if (it->opcode == opcode)
            {
                return &*it;
            }
        }
        return nullptr;
    }

    // ---- injecting packets ----
    void event(std::uint8_t code, const Bytes& params, std::uint32_t delayMs = 0);
    void acl(const Bytes& packet, std::uint32_t delayMs = 0);
    // an L2CAP frame for the connection `handle`, in one ACL packet
    void l2cap(std::uint16_t cid, const Bytes& payload, std::uint16_t handle = 0, std::uint32_t delayMs = 0);
    // the number of completed packets event for the remote's link
    void completedPackets(std::uint16_t handle, std::uint16_t count, std::uint32_t delayMs = 0);
    // the remote closes the link
    void remoteDisconnect(std::uint8_t reason = 0x13, std::uint32_t delayMs = 0);

    // called for every ACL packet the stack sends (a fake peer plugs in here)
    std::function<void(const Bytes&)> onAclOut;

    // Stop the clock jumping: pump() returns at the timeout it was given.
    bool pending() const
    {
        return !m_events.empty() || !m_acl.empty() || !m_scheduled.empty();
    }

  private:
    struct Scheduled
    {
        std::uint64_t at;
        bool isAcl;
        Bytes data;
    };

    static int take(std::deque<Bytes>& q, std::uint8_t* out, int max);
    void complete(std::uint16_t opcode, const Bytes& returnParams, std::uint32_t delayMs = 0);
    void status(std::uint16_t opcode, std::uint8_t code, std::uint32_t delayMs = 0);
    void respond(std::uint16_t opcode, const Bytes& params);
    void connectionComplete(std::uint8_t statusCode, std::uint32_t delayMs);
    void authenticated(std::uint32_t delayMs);

    FakeClock& m_clock;
    std::deque<Bytes> m_events, m_acl;
    std::vector<Scheduled> m_scheduled;
    bool m_linkUp = false;
};

} // namespace bluev::testing
