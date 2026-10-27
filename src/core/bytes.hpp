// SPDX-License-Identifier: GPL-3.0-or-later
// Multi-byte numbers in packets: HCI and L2CAP are little-endian, SDP and RTP
// are big-endian.
#pragma once

#include <cstdint>

namespace bluev
{

inline void putLe16(std::uint8_t* p, unsigned value)
{
    p[0] = static_cast<std::uint8_t>(value & 0xff);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

inline std::uint16_t getLe16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

inline std::uint32_t getLe24(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16);
}

inline void putBe16(std::uint8_t* p, unsigned value)
{
    p[0] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    p[1] = static_cast<std::uint8_t>(value & 0xff);
}

inline void putBe32(std::uint8_t* p, std::uint32_t value)
{
    putBe16(p, value >> 16);
    putBe16(p + 2, value & 0xffff);
}

inline std::uint16_t getBe16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

inline std::uint32_t getBe32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

} // namespace bluev
