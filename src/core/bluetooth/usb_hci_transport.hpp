// SPDX-License-Identifier: GPL-3.0-or-later
// HCI over a USB Bluetooth controller (class e0/01/01 or the layout of the
// console's own chip), on top of an IUsbDevice:
//
//   transfers 0..47   async event reads
//   transfers 48..63  async ACL reads
//   transfer  64      ACL out
//
// HCI commands go out as control requests. Completed reads are split into
// individual HCI packets and land in two queues. The traffic of the system's
// own stack on the console's chip arrives here too and is left to the layer
// above to ignore.
#pragma once

#include <cstdint>
#include <string>

#include "core/bluetooth/hci_packet_queue.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/hci_transport.hpp"
#include "core/ports/usb_device.hpp"

namespace bluev
{

class UsbHciTransport final : public IHciTransport
{
  public:
    // `consoleNode`: the node used when select() names none (the console's own chip).
    UsbHciTransport(IUsbDevice& device, IClock& clock, const char* consoleNode);
    ~UsbHciTransport() override;
    UsbHciTransport(const UsbHciTransport&) = delete;
    UsbHciTransport& operator=(const UsbHciTransport&) = delete;

    void select(const char* node, const HciLayout& layout) override;
    bool open() override;
    void close() override;
    bool command(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length) override;
    bool pump(std::uint32_t timeoutMs) override;
    int nextEvent(std::uint8_t* out, int max) override;
    int nextAcl(std::uint8_t* out, int max) override;
    bool sendAcl(const std::uint8_t* packet, std::uint32_t length) override;
    int detectRoles(std::uint32_t timeoutMs) override;
    std::string describe(const char* why) override;

  private:
    static constexpr int kEventFirst = 0, kEventLast = 47; // inclusive
    static constexpr int kAclFirst = 48, kAclLast = 63;
    static constexpr int kAclOut = 64;
    static constexpr int kEventBuf = 0x104;

    std::uint32_t reap();
    void onTransferComplete(int t);
    bool openTransfer(int transfer, std::uint8_t endpoint, std::uint32_t length);
    int openTransfers(int first, int last, std::uint8_t endpoint, std::uint32_t length);
    void closeDevice();
    void armReads(int first, int count, std::uint32_t length);

    IUsbDevice& m_device;
    IClock& m_clock;
    bool m_open = false;
    std::string m_consoleNode;
    std::string m_node;
    HciLayout m_layout{0, 0x82, 0x81, 0x01}; // until select(): how the console's chip is laid out on most units
    std::uint32_t m_eventLength = kEventBuf; // buffer of the "event" transfers

    // endpoint role detection, see detectRoles()
    bool m_detecting = false;
    int m_detected = 0;

    // counters for describe()
    unsigned m_dgComplete = 0, m_dgData = 0, m_dgEmpty = 0, m_dgError = 0, m_dgStartError = 0;
    unsigned m_dgCommands = 0, m_dgEventData = 0, m_dgAclData = 0;

    bool m_pending[kUsbTransfers] = {}; // started and not yet complete
    int m_eventReads = 0;
    int m_aclReads = 0;
    HciPacketQueue m_events;
    HciPacketQueue m_acl;
};

} // namespace bluev
