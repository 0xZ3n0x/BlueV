// SPDX-License-Identifier: GPL-3.0-or-later
// The RTP header of an A2DP media packet (payload type 96, one SSRC), and the
// SBC payload header after it.
#pragma once

#include <cstdint>

namespace bluev
{

constexpr int kRtpHeaderBytes = 12;
constexpr int kMediaHeaderBytes = 13; // RTP header + the SBC frame count

// Writes the 13 bytes in front of the SBC frames. `pkt` must hold at least
// kMediaHeaderBytes. Sequence and timestamp are cut to 16 and 32 bits.
inline void writeMediaHeader(std::uint8_t* pkt, std::uint32_t sequence, std::uint32_t timestamp, std::uint8_t sbcFrames)
{
    pkt[0] = 0x80; // RTP v2
    pkt[1] = 0x60; // payload type 96
    pkt[2] = static_cast<std::uint8_t>(sequence >> 8);
    pkt[3] = static_cast<std::uint8_t>(sequence);
    pkt[4] = static_cast<std::uint8_t>(timestamp >> 24);
    pkt[5] = static_cast<std::uint8_t>(timestamp >> 16);
    pkt[6] = static_cast<std::uint8_t>(timestamp >> 8);
    pkt[7] = static_cast<std::uint8_t>(timestamp);
    pkt[8] = 0;
    pkt[9] = 0;
    pkt[10] = 0;
    pkt[11] = 1; // ssrc
    pkt[12] = sbcFrames;
}

} // namespace bluev
