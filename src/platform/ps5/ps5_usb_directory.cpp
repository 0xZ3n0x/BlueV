// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/ps5_usb_directory.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include "core/bluetooth/usb_layout.hpp"

namespace bluev
{

namespace
{

// The ioctl that fills a description of the device behind a ugen node (read
// only; nothing is sent to the device). The console's answer is 8 bytes longer
// than the SDK's struct usb_device_info, so its number is not USB_DEVICEINFO
// (0xc1705504) but 0xc1785504; the fields we read are where the SDK has them.
constexpr unsigned long kUsbGetDeviceInfo = 0xc1785504UL;

struct DeviceInfoAnswer
{
    usb_device_info info;
    std::uint8_t more[0x180 - sizeof(usb_device_info)];
};

// The device behind /dev/<name>: its ids, class and strings. False when it
// cannot be opened.
bool readInfo(const char* name, UsbDevice& out)
{
    char path[48];
    DeviceInfoAnswer answer;

    std::snprintf(path, sizeof path, "/dev/%s", name);
    const int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        return false;
    }
    std::memset(&answer, 0, sizeof answer);
    const bool ok = ioctl(fd, kUsbGetDeviceInfo, &answer) == 0;
    if (ok)
    {
        out = UsbDevice{};
        out.node = name;
        out.pid = answer.info.udi_productNo;
        out.vid = answer.info.udi_vendorNo;
        out.deviceClass = answer.info.udi_class;
        out.subClass = answer.info.udi_subclass;
        out.protocol = answer.info.udi_protocol;
        char text[64];
        std::snprintf(text, sizeof text, "%.63s", answer.info.udi_product);
        out.product = text;
        std::snprintf(text, sizeof text, "%.63s", answer.info.udi_vendor);
        out.vendor = text;
    }
    close(fd);
    return ok;
}

// The whole configuration descriptor of the device behind /dev/<name>, taken
// from the kernel's copy (a control transfer for it is refused by ugen).
// Returns its length, 0 when it cannot be read. Read only, nothing is sent to
// the device.
int readConfigDescriptor(const char* name, std::uint8_t* buffer, std::uint16_t size)
{
    char path[48];
    usb_gen_descriptor request;
    int length = 0;

    std::snprintf(path, sizeof path, "/dev/%s", name);
    const int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        return 0;
    }
    std::memset(&request, 0, sizeof request);
    request.ugd_data = buffer;
    request.ugd_maxlen = size;
    request.ugd_config_index = 0;
    if (ioctl(fd, USB_GET_FULL_DESC, &request) == 0)
    {
        length = request.ugd_actlen;
    }
    close(fd);
    return length;
}

} // namespace

bool Ps5UsbDirectory::nodeInfo(const char* node, UsbDevice& out)
{
    return readInfo(node, out);
}

// The console's own Bluetooth chip (/dev/ugen0.2) is not touched and not
// listed; every other ugen device node that can be opened is. Listing only
// reads the device information.
std::vector<UsbDevice> Ps5UsbDirectory::adapters()
{
    std::vector<UsbDevice> found;
    DIR* d = opendir("/dev");

    if (d == nullptr)
    {
        return found;
    }
    while (dirent* de = readdir(d))
    {
        const std::size_t len = std::strlen(de->d_name);
        UsbDevice device;

        if (found.size() >= 8)
        {
            break;
        }
        if (std::strncmp(de->d_name, "ugen", 4) != 0 || len >= 24)
        {
            continue;
        }
        if (std::strcmp(de->d_name, internalNode()) == 0)
        {
            continue; // the console's own chip
        }
        if (len > 2 && std::strcmp(de->d_name + len - 2, ".1") == 0)
        {
            continue; // root hubs
        }
        if (readInfo(de->d_name, device))
        {
            found.push_back(std::move(device));
        }
    }
    closedir(d);
    return found;
}

bool Ps5UsbDirectory::discoverLayout(const char* node, HciLayout& out)
{
    std::uint8_t config[1024];
    const int length = readConfigDescriptor(node, config, sizeof config);

    return length > 0 && findHciLayout(config, length, out);
}

} // namespace bluev
