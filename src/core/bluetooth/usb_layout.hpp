// SPDX-License-Identifier: GPL-3.0-or-later
// Where a USB Bluetooth controller keeps its HCI endpoints, read from its
// configuration descriptor.
#pragma once

#include <cstdint>

#include "core/ports/hci_transport.hpp"

namespace bluev
{

// `config` is a whole configuration descriptor (the configuration, then its
// interfaces and their endpoints, `length` bytes in all). Finds the first
// interface (alternate setting 0) of class e0/01/01, the Bluetooth interface,
// that has an interrupt in, a bulk in and a bulk out endpoint: events = the
// first interrupt in, ACL in = the first bulk in, ACL out = the bulk out with
// the same endpoint number as the ACL in, else the first bulk out. False when
// there is none or the descriptor is cut short or malformed.
bool findHciLayout(const std::uint8_t* config, int length, HciLayout& out);

} // namespace bluev
