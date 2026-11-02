// SPDX-License-Identifier: GPL-3.0-or-later
// A USB device through FreeBSD's ugen (USB generic) driver and its USB_FS
// ioctls: a table of asynchronous transfers whose buffers the kernel writes
// into. An instance must not move or die while the device is open (close()
// takes the table down).
//
// A Bluetooth controller is a node under /dev: the console's own chip is
// ugen0.2, an adapter in a USB port has its own (ugen1.2 and so on).
#pragma once

#include <cstdint>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "core/ports/usb_device.hpp"

namespace bluev
{

class UgenDevice final : public IUsbDevice
{
  public:
    UgenDevice();
    ~UgenDevice() override;
    UgenDevice(const UgenDevice&) = delete;
    UgenDevice& operator=(const UgenDevice&) = delete;

    bool open(const char* node, int transfers, int forceShort) override;
    void close() override;
    bool openTransfer(int index, std::uint8_t endpoint, std::uint32_t length) override;
    std::uint8_t* buffer(int index) override;
    bool start(int index, std::uint32_t length) override;
    bool stop(int index) override;
    int complete() override;
    UsbTransferResult result(int index) override;
    bool control(std::uint8_t interface, const std::uint8_t* data, std::uint16_t length) override;
    void wait(std::uint32_t microseconds) override;
    int lastError() const override
    {
        return m_error;
    }

  private:
    int m_fd = -1;
    int m_error = 0;
    usb_fs_endpoint m_eps[kUsbTransfers]; // what the kernel reads and fills in, see USB_FS_INIT
    std::uint8_t m_buf[kUsbTransfers][kUsbTransferBytes];
    std::uint8_t* m_bufPtr[kUsbTransfers];
    std::uint32_t m_bufLen[kUsbTransfers];
};

} // namespace bluev
