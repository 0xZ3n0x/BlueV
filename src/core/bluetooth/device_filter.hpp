// SPDX-License-Identifier: GPL-3.0-or-later
// What an inquiry response says about a device: its name from the extended
// inquiry data, and whether it is a TV or the like rather than something to
// stream audio to.
#pragma once

#include <cstdint>
#include <string>

namespace bluev
{

// The complete or shortened name (types 0x08/0x09) of the extended inquiry
// data; empty when there is none. At most 63 characters.
std::string eirName(const std::uint8_t* eir, int length);

// A TV, soundbar and the like: by the minor class of the device or by a brand
// in its name.
bool isJunkDevice(std::uint32_t classOfDevice, const std::string& name);

} // namespace bluev
