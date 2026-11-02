// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/ps5_usb_directory.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <libusb.h> // structure layouts only; the calls go through libSceUsbd
#include <sys/ioctl.h>
#include <unistd.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include "core/ports/log.hpp"
#include "platform/ps5/ps5_api.hpp"

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

// The entry point of a library function by its NID.
void* resolve(int module, const char* nid)
{
    return reinterpret_cast<void*>(kernel_dynlib_resolve(getpid(), static_cast<uint32_t>(module), nid));
}

// The endpoints of a Bluetooth interface: the interrupt IN for events, the bulk
// IN for ACL data and the bulk OUT for ACL data, which is the one with the
// number of the bulk IN when there is such (0x82/0x02 on the Slim, 0x81/0x01 on
// the fat console). False when one of the three is missing.
bool readEndpoints(const libusb_interface_descriptor& id, HciLayout& out)
{
    int events = -1, aclIn = -1, aclOut = -1, aclOutPaired = -1;

    for (int e = 0; e < id.bNumEndpoints; e++)
    {
        const libusb_endpoint_descriptor& ep = id.endpoint[e];
        const int type = ep.bmAttributes & 3;
        const bool isIn = (ep.bEndpointAddress & 0x80) != 0;

        if (type == 3 && isIn && events < 0)
        {
            events = ep.bEndpointAddress;
        }
        else if (type == 2 && isIn && aclIn < 0)
        {
            aclIn = ep.bEndpointAddress;
        }
        else if (type == 2 && !isIn && aclOut < 0)
        {
            aclOut = ep.bEndpointAddress;
        }
    }
    for (int e = 0; e < id.bNumEndpoints; e++)
    {
        const libusb_endpoint_descriptor& ep = id.endpoint[e];

        if ((ep.bmAttributes & 3) == 2 && !(ep.bEndpointAddress & 0x80) && aclIn >= 0 &&
            (ep.bEndpointAddress & 0x0f) == (aclIn & 0x0f))
        {
            aclOutPaired = ep.bEndpointAddress;
        }
    }
    if (aclOutPaired >= 0)
    {
        aclOut = aclOutPaired;
    }
    if (events < 0 || aclIn < 0 || aclOut < 0)
    {
        return false;
    }
    out.interface = id.bInterfaceNumber;
    out.events = static_cast<std::uint8_t>(events);
    out.aclIn = static_cast<std::uint8_t>(aclIn);
    out.aclOut = static_cast<std::uint8_t>(aclOut);
    return true;
}

// The first Bluetooth interface (class e0, subclass 1, protocol 1, alternate
// setting 0) of a configuration that has the endpoints HCI needs.
bool findHciInterface(const libusb_config_descriptor& cfg, HciLayout& out)
{
    for (int n = 0; n < cfg.bNumInterfaces; n++)
    {
        for (int a = 0; a < cfg.interface[n].num_altsetting; a++)
        {
            const libusb_interface_descriptor& id = cfg.interface[n].altsetting[a];

            if (id.bAlternateSetting == 0 && id.bInterfaceClass == 0xe0 && id.bInterfaceSubClass == 1 &&
                id.bInterfaceProtocol == 1 && readEndpoints(id, out))
            {
                return true;
            }
        }
    }
    return false;
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

bool Ps5UsbDirectory::loadUsbd()
{
    int unused = 0;

    if (m_module != -2)
    {
        return m_module >= 0;
    }
    m_module = sceKernelLoadStartModule("/system/common/lib/libSceUsbd.sprx", 0, nullptr, 0, nullptr, &unused);
    if (m_module < 0)
    {
        logLine("usb: libSceUsbd could not be loaded (%d)", m_module);
        m_module = -1;
        return false;
    }
    m_init = reinterpret_cast<int (*)()>(resolve(m_module, "TOhg7P6kTH4"));
    m_exit = reinterpret_cast<void (*)()>(resolve(m_module, "Fq6+0Fm55xU"));
    m_list = reinterpret_cast<long (*)(void***)>(resolve(m_module, "8qB9Ar4P5nc"));
    m_freeList = reinterpret_cast<void (*)(void**, int)>(resolve(m_module, "EQ6SCLMqzkM"));
    m_bus = reinterpret_cast<int (*)(void*)>(resolve(m_module, "t7WE9mb1TB8"));
    m_address = reinterpret_cast<int (*)(void*)>(resolve(m_module, "GjlCrU4GcIY"));
    m_config = reinterpret_cast<int (*)(void*, void**)>(resolve(m_module, "S1o1C6yOt5g"));
    m_freeConfig = reinterpret_cast<void (*)(void*)>(resolve(m_module, "Hvd3S--n25w"));
    if (!m_init || !m_list || !m_freeList || !m_bus || !m_address || !m_config || !m_freeConfig)
    {
        logLine("usb: libSceUsbd is missing a function");
        m_module = -1;
        return false;
    }
    return true;
}

bool Ps5UsbDirectory::discoverLayout(const char* node, HciLayout& out)
{
    int bus = -1, address = -1;
    bool found = false;
    void** list = nullptr;

    if (std::sscanf(node, "ugen%d.%d", &bus, &address) != 2 || !loadUsbd())
    {
        return false;
    }
    if (m_init() != 0)
    {
        return false;
    }
    const long count = m_list(&list);
    for (long i = 0; i < count && list != nullptr && !found; i++)
    {
        libusb_config_descriptor* cfg = nullptr;

        if (m_bus(list[i]) != bus || m_address(list[i]) != address)
        {
            continue;
        }
        if (m_config(list[i], reinterpret_cast<void**>(&cfg)) != 0 || cfg == nullptr)
        {
            break;
        }
        found = findHciInterface(*cfg, out);
        m_freeConfig(cfg);
    }
    if (list != nullptr)
    {
        m_freeList(list, 1);
    }
    if (m_exit)
    {
        m_exit();
    }
    return found;
}

} // namespace bluev
