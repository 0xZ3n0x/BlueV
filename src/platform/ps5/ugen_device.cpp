// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/ugen_device.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/ports/log.hpp"

namespace bluev
{

UgenDevice::UgenDevice()
{
    std::memset(m_eps, 0, sizeof m_eps);
    std::memset(m_bufPtr, 0, sizeof m_bufPtr);
    std::memset(m_bufLen, 0, sizeof m_bufLen);
}

UgenDevice::~UgenDevice()
{
    close();
}

bool UgenDevice::open(const char* node, int transfers, int forceShort)
{
    usb_fs_init init;
    char path[48];

    std::snprintf(path, sizeof path, "/dev/%s", node);
    m_fd = ::open(path, O_RDWR);
    if (m_fd < 0)
    {
        m_error = errno;
        logLine("hci: open %s errno=%d", path, m_error);
        return false;
    }

    std::memset(m_eps, 0, sizeof m_eps);
    for (int i = 0; i < kUsbTransfers; i++)
    {
        m_bufPtr[i] = m_buf[i];
        m_eps[i].ppBuffer = reinterpret_cast<void**>(&m_bufPtr[i]);
        m_eps[i].pLength = &m_bufLen[i];
        m_bufLen[i] = kUsbTransferBytes;
        m_eps[i].nFrames = 1;
        m_eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    }
    m_eps[forceShort].flags = USB_FS_FLAG_FORCE_SHORT;

    std::memset(&init, 0, sizeof init);
    init.pEndpoints = m_eps;
    init.ep_index_max = static_cast<std::uint8_t>(transfers);
    if (ioctl(m_fd, USB_FS_INIT, &init) != 0)
    {
        m_error = errno;
        logLine("hci: USB_FS_INIT errno=%d", m_error);
        close();
        return false;
    }
    return true;
}

void UgenDevice::close()
{
    if (m_fd < 0)
    {
        return;
    }
    // One call takes the whole transfer table down (its argument is a dummy byte),
    // as the console's own libusb does when it closes a device.
    std::uint8_t dummy = 0;

    ioctl(m_fd, USB_FS_UNINIT, &dummy);
    ::close(m_fd);
    m_fd = -1;
}

bool UgenDevice::openTransfer(int index, std::uint8_t endpoint, std::uint32_t length)
{
    usb_fs_open op;

    std::memset(&op, 0, sizeof op);
    op.max_bufsize = length;
    op.max_frames = 1;
    op.ep_index = static_cast<std::uint8_t>(index);
    op.ep_no = endpoint;
    if (ioctl(m_fd, USB_FS_OPEN, &op) != 0)
    {
        m_error = errno;
        return false;
    }
    return true;
}

std::uint8_t* UgenDevice::buffer(int index)
{
    return m_buf[index];
}

bool UgenDevice::start(int index, std::uint32_t length)
{
    std::uint8_t id = static_cast<std::uint8_t>(index);

    m_bufLen[index] = length;
    m_eps[index].nFrames = 1;
    m_eps[index].aFrames = 0;
    m_eps[index].status = 0;
    if (ioctl(m_fd, USB_FS_START, &id) != 0)
    {
        m_error = errno;
        return false;
    }
    return true;
}

bool UgenDevice::stop(int index)
{
    std::uint8_t id = static_cast<std::uint8_t>(index);

    return ioctl(m_fd, USB_FS_STOP, &id) == 0;
}

int UgenDevice::complete()
{
    std::uint8_t id = 0;

    if (ioctl(m_fd, USB_FS_COMPLETE, &id) != 0)
    {
        return -1;
    }
    return id;
}

UsbTransferResult UgenDevice::result(int index)
{
    UsbTransferResult result;

    result.status = static_cast<int>(m_eps[index].status);
    result.frames = m_eps[index].aFrames;
    result.length = m_bufLen[index];
    return result;
}

bool UgenDevice::control(std::uint8_t interface, const std::uint8_t* data, std::uint16_t length)
{
    usb_ctl_request setup;

    std::memset(&setup, 0, sizeof setup);
    setup.ucr_request.bmRequestType = UT_WRITE_CLASS_DEVICE;
    setup.ucr_request.bRequest = 0x00;
    USETW(setup.ucr_request.wIndex, interface);
    USETW(setup.ucr_request.wLength, length);
    setup.ucr_data = const_cast<std::uint8_t*>(data);
    if (ioctl(m_fd, USB_DO_REQUEST, &setup) != 0)
    {
        m_error = errno;
        return false;
    }
    return true;
}

void UgenDevice::wait(std::uint32_t microseconds)
{
    usleep(microseconds);
}

} // namespace bluev
