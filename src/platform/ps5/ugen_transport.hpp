// SPDX-License-Identifier: GPL-3.0-or-later
// Raw HCI over FreeBSD's ugen (USB generic) driver.
//
// A Bluetooth controller is a node under /dev: the console's own chip is
// ugen0.2, an adapter in a USB port has its own (ugen1.2 and so on). The
// console's chip is opened beside the system's own Bluetooth stack, which keeps
// using it. Either is driven with USB_FS ioctls:
//
//   transfers 0..47   async event reads
//   transfers 48..63  async ACL reads
//   transfer  64      ACL out
//
// HCI commands go out as control transfers. Completed reads are split into
// individual HCI packets and land in two software rings. The traffic of the
// system's own stack on the console's chip arrives here too and is left to the
// layer above to ignore.
//
// The kernel writes into the buffers of armed transfers, so an instance must
// not move or die while the device is open (close() stops everything).
#pragma once

#include <cstdint>
#include <string>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "core/ports/clock.hpp"
#include "core/ports/hci_transport.hpp"
#include "platform/ps5/ps5_usb_directory.hpp"

namespace bluev
{

class UgenTransport final : public IHciTransport
{
  public:
    explicit UgenTransport(IClock& clock);
    ~UgenTransport() override;
    UgenTransport(const UgenTransport&) = delete;
    UgenTransport& operator=(const UgenTransport&) = delete;

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
    static constexpr int kTransfers = 65;
    static constexpr int kEventFirst = 0, kEventLast = 47; // inclusive
    static constexpr int kAclFirst = 48, kAclLast = 63;
    static constexpr int kAclOut = 64;
    static constexpr int kXferBuf = 0x44c;
    static constexpr int kEventBuf = 0x104;
    static constexpr int kRingSlots = 32;

    // one software ring of HCI packets
    struct Ring
    {
        std::uint8_t buf[kRingSlots][kXferBuf];
        std::uint32_t len[kRingSlots];
        int head = 0;
        int count = 0;
    };

    std::uint32_t reap();
    void closeFd();
    static int ringPop(Ring& ring, std::uint8_t* out, int max);
    bool startTransfer(int t);
    void armReads(int first, int count, std::uint32_t length);

    IClock& m_clock;
    int m_fd = -1;
    std::string m_node = kConsoleBluetoothNode;
    HciLayout m_layout{0, 0x82, 0x81, 0x01}; // until select(): how the console's chip is laid out on most units
    std::uint32_t m_eventLength = kEventBuf; // buffer of the "event" transfers

    // endpoint role detection, see detectRoles()
    bool m_detecting = false;
    int m_detected = 0;

    // counters for describe()
    unsigned m_dgComplete = 0, m_dgData = 0, m_dgEmpty = 0, m_dgError = 0, m_dgStartError = 0;
    unsigned m_dgCommands = 0, m_dgEventData = 0, m_dgAclData = 0;

    usb_fs_endpoint m_eps[kTransfers]; // what the kernel reads and fills in, see USB_FS_INIT
    std::uint8_t m_buf[kTransfers][kXferBuf];
    std::uint8_t* m_bufPtr[kTransfers];
    std::uint32_t m_bufLen[kTransfers];
    std::uint32_t m_pending[kTransfers];
    int m_eventReads = 0;
    int m_aclReads = 0;
    Ring m_events;
    Ring m_acl;
};

} // namespace bluev
