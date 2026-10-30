// SPDX-License-Identifier: GPL-3.0-or-later
// Bytes as text for the log.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace bluev
{

// The first bytes of a frame as hex, "01 02 03 "; at most 24 of them.
inline std::string hexStart(const std::uint8_t* data, int length)
{
    std::string text;
    char byte[4];

    for (int i = 0; i < length && i < 24; i++)
    {
        std::snprintf(byte, sizeof(byte), "%02x ", data[i]);
        text += byte;
    }
    return text;
}

} // namespace bluev
