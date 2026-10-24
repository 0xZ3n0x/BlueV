// SPDX-License-Identifier: GPL-3.0-or-later
// The SBC parameters we stream with, chosen from what the device offers.
#pragma once

#include <cstdint>

namespace bluev
{

// numbering as libsbc uses it
constexpr std::uint32_t kSbcFreq44100 = 2;
constexpr std::uint32_t kSbcFreq48000 = 3;
constexpr std::uint32_t kSbcModeStereo = 2;
constexpr std::uint32_t kSbcModeJointStereo = 3;

struct A2dpConfig
{
    std::uint32_t rate = 0;    // 48000 or 44100
    std::uint8_t freqMode = 0; // A2DP byte 1: frequency bit | channel mode bit
    std::uint8_t cfg = 0;      // A2DP byte 2: blocks / subbands / allocation
    std::uint8_t poolMin = 0, poolMax = 0;
    std::uint32_t freqCode = 0;   // libsbc frequency code
    std::uint32_t modeCode = 0;   // libsbc channel mode
    std::uint32_t blocks = 0;     // libsbc block code
    std::uint32_t subbands8 = 0;  // 1 = 8 subbands
    std::uint32_t allocation = 0; // libsbc allocation method
    std::uint32_t bitpool = 0;
};

// `caps` is the SBC codec element of the device: byte 1 | byte 2 << 8 |
// minimum bitpool << 16 | maximum bitpool << 24. Prefers 48 kHz, joint
// stereo, the longest blocks, 8 subbands, bitpool 40 within the device's
// range. False, with a log line, when the device takes neither 44.1 nor 48
// kHz or no stereo mode.
bool chooseA2dpParams(std::uint32_t caps, A2dpConfig& out);

} // namespace bluev
