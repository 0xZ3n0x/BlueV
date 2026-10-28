// SPDX-License-Identifier: GPL-3.0-or-later
// The USB devices of the console: which Bluetooth adapters are plugged in,
// and how a Bluetooth chip's endpoints are laid out.
//
// Everything comes from the ugen driver by ioctl: the device information, and
// the configuration descriptor (the kernel keeps a copy of it, ugen refuses a
// control transfer for it). Nothing is sent to the devices.
#pragma once

#include "core/ports/usb_directory.hpp"

namespace bluev
{

// the console's own Bluetooth chip
constexpr const char* kConsoleBluetoothNode = "ugen0.2";

class Ps5UsbDirectory final : public IUsbDirectory
{
  public:
    const char* internalNode() const override
    {
        return kConsoleBluetoothNode;
    }
    std::vector<UsbDevice> adapters() override;
    bool nodeInfo(const char* node, UsbDevice& out) override;
    // Reads the configuration descriptor and finds the Bluetooth interface in
    // it (see findHciLayout).
    bool discoverLayout(const char* node, HciLayout& out) override;
};

} // namespace bluev
