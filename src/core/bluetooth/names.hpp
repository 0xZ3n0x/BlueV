// SPDX-License-Identifier: GPL-3.0-or-later
// Names for the numbers in the log, so a line reads "page timeout" and not
// "0x04". Every function returns a static string and never null; an unknown
// number gets a generic text.
#pragma once

#include <cstdint>

namespace bluev
{

const char* hciStatusName(unsigned code);                 // HCI error code
const char* hciOpcodeName(std::uint16_t opcode);          // command opcode (OGF/OCF)
const char* deviceClassName(std::uint32_t classOfDevice); // major class of a class-of-device
const char* manufacturerName(unsigned id);                // company id from Read Local Version
const char* hciVersionName(unsigned version);             // "4.0" for 6, and so on

} // namespace bluev
