// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/usb_hci_transport.hpp"

#include <cstdio>
#include <cstring>

#include "core/ports/log.hpp"

namespace bluev
{

UsbHciTransport::UsbHciTransport(IUsbDevice& device, IClock& clock, const char* consoleNode)
    : m_device(device), m_clock(clock), m_consoleNode(consoleNode), m_node(consoleNode)
{
}

UsbHciTransport::~UsbHciTransport()
{
    closeDevice();
}

void UsbHciTransport::select(const char* node, const HciLayout& layout)
{
    m_node = node != nullptr && node[0] ? node : m_consoleNode;
    m_layout = layout;
    // an interrupt-style event pipe (0x82) is read with 0x104 bytes; the
    // others keep the proven 0x44c length
    m_eventLength = layout.events == 0x82 ? kEventBuf : kUsbTransferBytes;
}

// A transfer that is still running when the process ends would be written to by
// the kernel after the memory is gone, so every pending one is stopped first,
// then the transfer table is taken down.
void UsbHciTransport::closeDevice()
{
    if (!m_open)
    {
        return;
    }
    for (int i = 0; i < kUsbTransfers; i++)
    {
        if (m_pending[i])
        {
            m_device.stop(i);
        }
        m_pending[i] = false;
    }
    m_device.close();
    m_open = false;
}

// Opens transfer `transfer` on endpoint address `endpoint`: one frame of at
// most `length` bytes.
bool UsbHciTransport::openTransfer(int transfer, std::uint8_t endpoint, std::uint32_t length)
{
    return m_device.openTransfer(transfer, endpoint, length);
}

// Opens the transfers `first` to `last` on one endpoint; how many could be opened.
int UsbHciTransport::openTransfers(int first, int last, std::uint8_t endpoint, std::uint32_t length)
{
    int opened = 0;

    for (int i = first; i <= last; i++)
    {
        if (!openTransfer(i, endpoint, length))
        {
            logLine("hci: USB_FS_OPEN %#x (transfer %d) errno=%d", endpoint, i, m_device.lastError());
            break;
        }
        opened++;
    }
    return opened;
}

bool UsbHciTransport::open()
{
    m_dgComplete = m_dgData = m_dgEmpty = m_dgError = m_dgStartError = 0;
    m_dgCommands = m_dgEventData = m_dgAclData = 0;

    if (!m_device.open(m_node.c_str(), kUsbTransfers, kAclOut))
    {
        return false;
    }
    m_open = true;

    // ACL out endpoint
    if (!openTransfer(kAclOut, m_layout.aclOut, kUsbTransferBytes))
    {
        logLine("hci: USB_FS_OPEN %#x (transfer %d) errno=%d", m_layout.aclOut, kAclOut, m_device.lastError());
        closeDevice();
        return false;
    }
    m_eventReads = openTransfers(kEventFirst, kEventLast, m_layout.events, m_eventLength); // 48 event reads
    m_aclReads = openTransfers(kAclFirst, kAclLast, m_layout.aclIn, kUsbTransferBytes);    // 16 ACL reads

    if (m_aclReads != 0 && m_eventReads > 0)
    {
        std::memset(m_pending, 0, sizeof m_pending);
        m_events.clear();
        m_acl.clear();
        logLine("hci: controller open alongside the system, %d event and %d ACL reads (ep ev=%#x acl_in=%#x "
                "acl_out=%#x)",
                m_eventReads, m_aclReads, m_layout.events, m_layout.aclIn, m_layout.aclOut);
        return true;
    }
    closeDevice();
    return false;
}

void UsbHciTransport::close()
{
    if (m_open)
    {
        logLine("%s", describe("close").c_str());
    }
    closeDevice();
}

std::string UsbHciTransport::describe(const char* why)
{
    int pendingEvents = 0, pendingAcl = 0;
    char text[512];

    for (int i = kEventFirst; i < kEventFirst + m_eventReads; i++)
    {
        pendingEvents += m_pending[i] ? 1 : 0;
    }
    for (int i = kAclFirst; i < kAclFirst + m_aclReads; i++)
    {
        pendingAcl += m_pending[i] ? 1 : 0;
    }
    std::snprintf(text, sizeof text,
                  "hci (%s): endpoints ev=%#x acl_in=%#x acl_out=%#x; commands %u, "
                  "transfers %u (data %u, empty %u, errors %u), event packets %u, "
                  "ACL packets %u, start errors %u, armed ev=%d acl=%d, queued ev=%d acl=%d",
                  why, m_layout.events, m_layout.aclIn, m_layout.aclOut, m_dgCommands, m_dgComplete, m_dgData,
                  m_dgEmpty, m_dgError, m_dgEventData, m_dgAclData, m_dgStartError, pendingEvents, pendingAcl,
                  m_events.count(), m_acl.count());
    return text;
}

bool UsbHciTransport::command(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length)
{
    std::uint8_t pkt[4 + 255];

    pkt[0] = opcode & 0xff;
    pkt[1] = static_cast<std::uint8_t>(opcode >> 8);
    pkt[2] = length;
    if (length > 0)
    {
        std::memcpy(pkt + 3, params, length);
    }
    if (!m_device.control(m_layout.interface, pkt, static_cast<std::uint16_t>(length + 3)))
    {
        logLine("hci: command %#06x failed, errno=%d", opcode, m_device.lastError());
        return false;
    }
    m_dgCommands++;
    return true;
}

// Starts every one of the `count` read transfers from `first` on that is not
// running.
void UsbHciTransport::armReads(int first, int count, std::uint32_t length)
{
    for (int i = first; i < first + count; i++)
    {
        if (m_pending[i])
        {
            continue;
        }
        if (m_device.start(i, length))
        {
            m_pending[i] = true;
        }
        else
        {
            m_dgStartError++;
            logLine("hci: USB_FS_START %d errno=%d", i, m_device.lastError());
        }
    }
}

// Submits any idle async reads, then waits for something to land in a queue
// (true) or the timeout to expire (false).
bool UsbHciTransport::pump(std::uint32_t timeoutMs)
{
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;

    for (;;)
    {
        armReads(kEventFirst, m_eventReads, m_eventLength);
        armReads(kAclFirst, m_aclReads, kUsbTransferBytes);

        const std::uint32_t r = reap();

        if (m_events.count() != 0 || m_acl.count() != 0)
        {
            return true;
        }
        if (r == 0)
        {
            // nothing completed: pump for at most timeoutMs
            if (m_clock.nowMs() >= deadline)
            {
                return false;
            }
            m_device.wait(1000);
        }
    }
}

// reap completed transfers, split their buffers into HCI packets and push
// them onto the queues. Returns 1 when something was reaped.
std::uint32_t UsbHciTransport::reap()
{
    std::uint32_t reaped = 0;

    for (;;)
    {
        const int t = m_device.complete();

        if (t < 0)
        {
            return reaped;
        }
        reaped = 1;
        if (t < kUsbTransfers)
        {
            onTransferComplete(t);
        }
    }
}

// The kernel finished transfer `t`: count it, and queue the packets it carried.
void UsbHciTransport::onTransferComplete(int t)
{
    const UsbTransferResult result = m_device.result(t);

    m_dgComplete++;
    m_pending[t] = false;
    if (t == kAclOut)
    {
        if (result.status != 0)
        {
            logLine("hci: ACL out status=%d", result.status);
        }
        return;
    }
    if (result.status != 0)
    {
        m_dgError++;
        return;
    }
    if (result.frames == 0)
    {
        m_dgEmpty++;
        return;
    }
    m_dgData++;

    const bool isEvent = t <= kEventLast;
    const std::uint32_t len = result.length;
    const std::uint8_t* buf = m_device.buffer(t);

    if (isEvent)
    {
        m_dgEventData++;
    }
    else
    {
        m_dgAclData++;
    }
    // While detecting roles, the answer to our Read Buffer Size command (a
    // Command Complete for opcode 0x1005) shows which pipe carries events.
    if (m_detecting && m_detected == 0 && isReadBufferSizeComplete(buf, len))
    {
        m_detected = isEvent ? 1 : 2;
    }
    // a transfer may hold several packets back to back
    (isEvent ? m_events : m_acl).pushTransfer(buf, len, isEvent ? HciPacketKind::Event : HciPacketKind::Acl);
}

bool UsbHciTransport::sendAcl(const std::uint8_t* packet, std::uint32_t length)
{
    if (length >= static_cast<std::uint32_t>(kUsbTransferBytes) + 1)
    {
        logLine("hci: packet of %u bytes exceeds the transfer buffer", length);
        return false;
    }

    const std::uint64_t deadline = m_clock.nowMs() + 1000;
    while (m_pending[kAclOut])
    {
        reap();
        if (!m_pending[kAclOut])
        {
            break;
        }
        if (m_clock.nowMs() >= deadline)
        {
            logLine("hci: ACL out still busy after 1 s");
            return false;
        }
        m_device.wait(500);
    }

    std::memcpy(m_device.buffer(kAclOut), packet, length);
    if (!m_device.start(kAclOut, length))
    {
        logLine("hci: USB_FS_START %d errno=%d", kAclOut, m_device.lastError());
        return false;
    }
    m_pending[kAclOut] = true;
    return true;
}

int UsbHciTransport::nextEvent(std::uint8_t* out, int max)
{
    return m_events.pop(out, max);
}

int UsbHciTransport::nextAcl(std::uint8_t* out, int max)
{
    return m_acl.pop(out, max);
}

// Find out which of the two IN endpoints carries the HCI events: send Read
// Buffer Size and see on which transfer group its Command Complete shows up.
// 1 if it arrives on the first (events) endpoint, 2 if on the second (ACL in),
// 0 if nothing came back.
int UsbHciTransport::detectRoles(std::uint32_t timeoutMs)
{
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;
    std::uint64_t nextCommand = 0;
    std::uint8_t scratch[kUsbTransferBytes];

    m_detected = 0;
    m_detecting = true;
    while (m_clock.nowMs() < deadline && m_detected == 0)
    {
        if (m_clock.nowMs() >= nextCommand)
        {
            command(0x1005, nullptr, 0);
            nextCommand = m_clock.nowMs() + 1500;
        }
        pump(30);
        while (nextEvent(scratch, sizeof scratch) > 0)
            ;
        while (nextAcl(scratch, sizeof scratch) > 0)
            ;
    }
    m_detecting = false;
    logLine("hci: role detection -> %d (ev ep %#x, 2nd ep %#x)", m_detected, m_layout.events, m_layout.aclIn);
    logLine("%s", describe("detect").c_str());
    return m_detected;
}

} // namespace bluev
