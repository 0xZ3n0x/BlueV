// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/a2dp_params.hpp"

#include "core/ports/log.hpp"

namespace bluev
{

bool chooseA2dpParams(std::uint32_t caps, A2dpConfig& c)
{
    c = A2dpConfig{};

    // sampling frequency: prefer 48 kHz
    if (caps & 0x10)
    {
        c.rate = 48000;
        c.freqCode = kSbcFreq48000;
        c.freqMode = 0x10;
    }
    else if (caps & 0x20)
    {
        c.rate = 44100;
        c.freqCode = kSbcFreq44100;
        c.freqMode = 0x20;
    }
    else
    {
        logLine("sbc: the device takes neither 44.1 nor 48 kHz");
        return false;
    }
    // channel mode: prefer joint stereo
    if (caps & 0x01)
    {
        c.modeCode = kSbcModeJointStereo;
        c.freqMode |= 0x01;
    }
    else if (caps & 0x02)
    {
        c.modeCode = kSbcModeStereo;
        c.freqMode |= 0x02;
    }
    else
    {
        logLine("sbc: the device takes no stereo mode");
        return false;
    }
    // The device's second byte: block length in bits 7..4 (one bit each, the
    // longest offered is taken), subbands in bits 3..2 (8 = 0x04, 4 = 0x08) and
    // allocation in bits 1..0 (loudness = 0x01, SNR = 0x02).
    if ((caps >> 8) & 0x10)
    {
        c.blocks = 3; // 16
        c.cfg = 0x10;
    }
    else if ((caps >> 8) & 0x20)
    {
        c.blocks = 2; // 12
        c.cfg = 0x20;
    }
    else if ((caps >> 8) & 0x40)
    {
        c.blocks = 1; // 8
        c.cfg = 0x40;
    }
    else
    {
        c.blocks = 0; // 4
        c.cfg = 0x80;
    }
    c.subbands8 = ((caps >> 8) & 0x04) >> 2; // 8 subbands when offered
    c.allocation = ((caps >> 8) & 1) ^ 1;    // libsbc: 0 = loudness, 1 = SNR; loudness when offered
    // the low nibble of the answer: 0x04 + 0x02 (8 subbands, SNR), changed to
    // 4 subbands (+4) and to loudness (-1) where those are what is chosen
    c.cfg = static_cast<std::uint8_t>(c.cfg + 6 + (((caps >> 8) & 4) ? 0 : 4) - ((caps >> 8) & 1));
    // bitpool: locked at 40, clamped to the sink's range
    c.poolMin = static_cast<std::uint8_t>(caps >> 16);
    c.poolMax = static_cast<std::uint8_t>(caps >> 24);
    c.bitpool = 40;
    if (c.poolMax < 40)
    {
        c.bitpool = c.poolMax;
    }
    if (c.bitpool <= c.poolMin)
    {
        c.bitpool = c.poolMin;
    }
    return true;
}

} // namespace bluev
