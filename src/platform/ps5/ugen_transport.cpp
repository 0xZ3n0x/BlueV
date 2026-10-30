// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/ugen_transport.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/ports/log.hpp"

namespace bluev
{

UgenTransport::UgenTransport(IClock& clock) : m_clock(clock)
{
    std::memset(m_eps, 0, sizeof m_eps);
    std::memset(m_bufPtr, 0, sizeof m_bufPtr);
    std::memset(m_bufLen, 0, sizeof m_bufLen);
    std::memset(m_pending, 0, sizeof m_pending);
}

UgenTransport::~UgenTransport()
{
    closeFd();
}

void UgenTransport::select(const char* node, const HciLayout& layout)
{
    m_node = node != nullptr && node[0] ? node : kConsoleBluetoothNode;
    m_layout = layout;
    // an interrupt-style event pipe (0x82) is read with 0x104 bytes; the
    // others keep the proven 0x44c length
    m_eventLength = layout.events == 0x82 ? kEventBuf : kXferBuf;
}

// A transfer that is still armed when the process ends would be written to by
// the kernel after the memory is gone, so every pending one is stopped first,
// then the transfer table is taken down.
void UgenTransport::closeFd()
{
    if (m_fd < 0)
    {
        return;
    }
    for (int i = 0; i < kTransfers; i++)
    {
        if (m_pending[i])
        {
            std::uint8_t t = static_cast<std::uint8_t>(i);
            ioctl(m_fd, USB_FS_STOP, &t);
        }
        m_pending[i] = 0;
    }
    // One call takes the whole transfer table down (its argument is a dummy byte),
    // as the console's own libusb does when it closes a device.
    std::uint8_t dummy = 0;
    ioctl(m_fd, USB_FS_UNINIT, &dummy);
    ::close(m_fd);
    m_fd = -1;
}

// Opens transfer `transfer` on endpoint address `endpoint`: one frame of at
// most `length` bytes.
bool UgenTransport::openTransfer(int transfer, std::uint8_t endpoint, std::uint32_t length)
{
    usb_fs_open op;

    std::memset(&op, 0, sizeof op);
    op.max_bufsize = length;
    op.max_frames = 1;
    op.ep_index = static_cast<std::uint8_t>(transfer);
    op.ep_no = endpoint;
    return ioctl(m_fd, USB_FS_OPEN, &op) == 0;
}

// Opens the transfers `first` to `last` on one endpoint; how many could be opened.
int UgenTransport::openTransfers(int first, int last, std::uint8_t endpoint, std::uint32_t length)
{
    int opened = 0;

    for (int i = first; i <= last; i++)
    {
        if (!openTransfer(i, endpoint, length))
        {
            logLine("hci: USB_FS_OPEN %#x (transfer %d) errno=%d", endpoint, i, errno);
            break;
        }
        opened++;
    }
    return opened;
}

bool UgenTransport::open()
{
    usb_fs_init init;
    char path[48];

    m_dgComplete = m_dgData = m_dgEmpty = m_dgError = m_dgStartError = 0;
    m_dgCommands = m_dgEventData = m_dgAclData = 0;

    std::snprintf(path, sizeof path, "/dev/%s", m_node.c_str());
    m_fd = ::open(path, O_RDWR);
    if (m_fd < 0)
    {
        logLine("hci: open %s errno=%d", path, errno);
        return false;
    }

    std::memset(m_eps, 0, sizeof m_eps);
    for (int i = 0; i < kTransfers; i++)
    {
        m_bufPtr[i] = m_buf[i];
        m_eps[i].ppBuffer = reinterpret_cast<void**>(&m_bufPtr[i]);
        m_eps[i].pLength = &m_bufLen[i];
        m_bufLen[i] = kXferBuf;
        m_eps[i].nFrames = 1;
        m_eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    }
    m_eps[kAclOut].flags = USB_FS_FLAG_FORCE_SHORT;

    std::memset(&init, 0, sizeof init);
    init.pEndpoints = m_eps;
    init.ep_index_max = kTransfers;
    if (ioctl(m_fd, USB_FS_INIT, &init) != 0)
    {
        logLine("hci: USB_FS_INIT errno=%d", errno);
        closeFd();
        return false;
    }

    // ACL out endpoint
    if (!openTransfer(kAclOut, m_layout.aclOut, kXferBuf))
    {
        logLine("hci: USB_FS_OPEN %#x (transfer %d) errno=%d", m_layout.aclOut, kAclOut, errno);
        closeFd();
        return false;
    }
    m_eventReads = openTransfers(kEventFirst, kEventLast, m_layout.events, m_eventLength); // 48 event reads
    m_aclReads = openTransfers(kAclFirst, kAclLast, m_layout.aclIn, kXferBuf);             // 16 ACL reads

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
    closeFd();
    return false;
}

void UgenTransport::close()
{
    if (m_fd >= 0)
    {
        logLine("%s", describe("close").c_str());
    }
    closeFd();
}

std::string UgenTransport::describe(const char* why)
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

bool UgenTransport::command(std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length)
{
    std::uint8_t pkt[4 + 255];
    usb_ctl_request setup;

    pkt[0] = opcode & 0xff;
    pkt[1] = static_cast<std::uint8_t>(opcode >> 8);
    pkt[2] = length;
    if (length > 0)
    {
        std::memcpy(pkt + 3, params, length);
    }

    std::memset(&setup, 0, sizeof setup);
    setup.ucr_request.bmRequestType = UT_WRITE_CLASS_DEVICE;
    setup.ucr_request.bRequest = 0x00;
    USETW(setup.ucr_request.wIndex, m_layout.interface);
    USETW(setup.ucr_request.wLength, length + 3);
    setup.ucr_data = pkt;

    if (ioctl(m_fd, USB_DO_REQUEST, &setup) != 0)
    {
        logLine("hci: command %#06x failed, errno=%d", opcode, errno);
        return false;
    }
    m_dgCommands++;
    return true;
}

bool UgenTransport::startTransfer(int t)
{
    std::uint8_t id = static_cast<std::uint8_t>(t);

    m_eps[t].nFrames = 1;
    m_eps[t].aFrames = 0;
    m_eps[t].status = 0;
    return ioctl(m_fd, USB_FS_START, &id) == 0;
}

// Starts every one of the `count` read transfers from `first` on that is not
// running.
void UgenTransport::armReads(int first, int count, std::uint32_t length)
{
    for (int i = first; i < first + count; i++)
    {
        if (m_pending[i])
        {
            continue;
        }
        m_bufLen[i] = length;
        if (startTransfer(i))
        {
            m_pending[i] = 1;
        }
        else
        {
            m_dgStartError++;
            logLine("hci: USB_FS_START %d errno=%d", i, errno);
        }
    }
}

// Submits any idle async reads, then waits for something to land in a ring
// (true) or the timeout to expire (false).
bool UgenTransport::pump(std::uint32_t timeoutMs)
{
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;

    for (;;)
    {
        armReads(kEventFirst, m_eventReads, m_eventLength);
        armReads(kAclFirst, m_aclReads, kXferBuf);

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
            usleep(1000);
        }
    }
}

// reap completed transfers, split their buffers into HCI packets and push
// them onto the rings. Returns 1 when something was reaped.
std::uint32_t UgenTransport::reap()
{
    std::uint32_t reaped = 0;

    for (;;)
    {
        std::uint8_t t = 0;

        if (ioctl(m_fd, USB_FS_COMPLETE, &t) != 0)
        {
            return reaped;
        }
        reaped = 1;
        if (t < kTransfers)
        {
            onTransferComplete(t);
        }
    }
}

// The kernel finished transfer `t`: count it, and queue the packets it carried.
void UgenTransport::onTransferComplete(int t)
{
    m_dgComplete++;
    m_pending[t] = 0;
    if (t == kAclOut)
    {
        if (m_eps[t].status != 0)
        {
            logLine("hci: ACL out status=%d", m_eps[t].status);
        }
        return;
    }
    if (m_eps[t].status != 0)
    {
        m_dgError++;
        return;
    }
    if (m_eps[t].aFrames == 0)
    {
        m_dgEmpty++;
        return;
    }
    m_dgData++;

    const bool isEvent = t <= kEventLast;
    const std::uint32_t len = m_bufLen[t];
    const std::uint8_t* buf = m_buf[t];

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

bool UgenTransport::sendAcl(const std::uint8_t* packet, std::uint32_t length)
{
    if (length >= static_cast<std::uint32_t>(kXferBuf) + 1)
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
        usleep(500);
    }

    std::memcpy(m_buf[kAclOut], packet, length);
    m_bufLen[kAclOut] = length;
    if (!startTransfer(kAclOut))
    {
        logLine("hci: USB_FS_START %d errno=%d", kAclOut, errno);
        return false;
    }
    m_pending[kAclOut] = 1;
    return true;
}

int UgenTransport::nextEvent(std::uint8_t* out, int max)
{
    return m_events.pop(out, max);
}

int UgenTransport::nextAcl(std::uint8_t* out, int max)
{
    return m_acl.pop(out, max);
}

// Find out which of the two IN endpoints carries the HCI events: send Read
// Buffer Size and see on which transfer group its Command Complete shows up.
// 1 if it arrives on the first (events) endpoint, 2 if on the second (ACL in),
// 0 if nothing came back.
int UgenTransport::detectRoles(std::uint32_t timeoutMs)
{
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;
    std::uint64_t nextCommand = 0;
    std::uint8_t scratch[kXferBuf];

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
