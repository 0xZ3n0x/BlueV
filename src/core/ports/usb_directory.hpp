// SPDX-License-Identifier: GPL-3.0-or-later
// What is plugged into the console's USB ports, and how the console's own
// Bluetooth chip is laid out.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/ports/hci_transport.hpp"

namespace bluev
{

struct UsbDevice
{
    std::string node; // a name under /dev, e.g. "ugen1.2"
    std::uint16_t vid = 0, pid = 0;
    std::uint8_t deviceClass = 0, subClass = 0, protocol = 0; // e0/01/01 = Bluetooth
    std::string product, vendor;
};

class IUsbDirectory
{
  public:
    virtual ~IUsbDirectory() = default;

    // the node of the console's own chip
    virtual const char* internalNode() const = 0;
    // everything in the USB ports except the console's own chip
    virtual std::vector<UsbDevice> adapters() = 0;
    // one node, the console's own chip included
    virtual bool nodeInfo(const char* node, UsbDevice& out) = 0;
    // the Bluetooth interface and its endpoints, read from the descriptors
    virtual bool discoverLayout(const char* node, HciLayout& out) = 0;
};

} // namespace bluev
