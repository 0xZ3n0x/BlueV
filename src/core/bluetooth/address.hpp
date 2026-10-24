// SPDX-License-Identifier: GPL-3.0-or-later
// Bluetooth device addresses, kept the way HCI carries them (least significant
// byte first), and their text form "AA:BB:CC:DD:EE:FF".
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace bluev
{

using BdAddr = std::array<std::uint8_t, 6>;

// "AA:BB:CC:DD:EE:FF" in a small buffer, for log lines
struct AddrText
{
    char text[18];
    const char* c_str() const
    {
        return text;
    }
};

AddrText addressText(const BdAddr& address);
std::string addressString(const BdAddr& address);
// Reads "AA:BB:CC:DD:EE:FF" (either letter case). Returns false, leaving `out`
// alone, when the text is anything else, including text after the address.
bool parseAddress(std::string_view text, BdAddr& out);
BdAddr addressFrom(const std::uint8_t* six); // copies 6 bytes

} // namespace bluev
