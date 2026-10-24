// SPDX-License-Identifier: GPL-3.0-or-later
// The USB devices of the console: which Bluetooth adapters are plugged in,
// and how a Bluetooth chip's endpoints are laid out.
//
// The device information comes from the ugen driver. The endpoint layout
// comes from the configuration descriptor, read through libSceUsbd (the
// console's embedded libusb-1.0) because ugen's GET_DESCRIPTOR is blocked.
// Only this lookup uses libusb; everything else goes through /dev/ugen.
#pragma once

#include "core/ports/usb_directory.hpp"
#include "platform/ps5/console.hpp"

namespace bluev
{

class Ps5UsbDirectory final : public IUsbDirectory
{
  public:
    const char* internalNode() const override
    {
        return kConsoleBluetoothNode;
    }
    std::vector<UsbDevice> adapters() override;
    bool nodeInfo(const char* node, UsbDevice& out) override;
    // Finds the first interface of class e0/01/01 that has an interrupt in,
    // a bulk in and a bulk out endpoint: events = the first interrupt in,
    // ACL in = the first bulk in, ACL out = the bulk out with the same
    // endpoint number as the ACL in, else the first bulk out.
    bool discoverLayout(const char* node, HciLayout& out) override;

  private:
    bool loadUsbd();

    int (*m_init)() = nullptr;
    void (*m_exit)() = nullptr;
    long (*m_list)(void*** list) = nullptr;
    void (*m_freeList)(void** list, int unref) = nullptr;
    int (*m_bus)(void* dev) = nullptr;
    int (*m_address)(void* dev) = nullptr;
    int (*m_config)(void* dev, void** cfg) = nullptr;
    void (*m_freeConfig)(void* cfg) = nullptr;
    int m_module = -2; // module handle, -1 = unavailable
};

} // namespace bluev
