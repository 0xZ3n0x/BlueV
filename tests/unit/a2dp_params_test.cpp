// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "capture_sink.hpp"
#include "core/audio/a2dp_params.hpp"

using namespace bluev;

namespace
{
constexpr std::uint32_t caps(std::uint8_t b1, std::uint8_t b2, std::uint8_t lo, std::uint8_t hi)
{
    return b1 | b2 << 8 | static_cast<std::uint32_t>(lo) << 16 | static_cast<std::uint32_t>(hi) << 24;
}
} // namespace

TEST(A2dpParams, TheSpeakerThatTakesEverything)
{
    A2dpConfig c;
    ASSERT_TRUE(chooseA2dpParams(caps(0xff, 0xff, 2, 53), c)); // what the Creative and the Sony offer
    EXPECT_EQ(48000u, c.rate);
    EXPECT_EQ(kSbcFreq48000, c.freqCode);
    EXPECT_EQ(kSbcModeJointStereo, c.modeCode);
    EXPECT_EQ(3u, c.blocks); // 16 blocks
    EXPECT_EQ(1u, c.subbands8);
    EXPECT_EQ(0u, c.allocation); // loudness
    EXPECT_EQ(40u, c.bitpool);
    EXPECT_EQ(0x11, c.freqMode); // 48 kHz | joint stereo
    EXPECT_EQ(0x15, c.cfg);      // 16 blocks, 8 subbands, loudness
    EXPECT_EQ(2, c.poolMin);
    EXPECT_EQ(53, c.poolMax);
}

TEST(A2dpParams, Only44100AndStereo)
{
    A2dpConfig c;
    ASSERT_TRUE(chooseA2dpParams(caps(0x22, 0x15, 2, 53), c)); // the headset configuration seen in the logs
    EXPECT_EQ(44100u, c.rate);
    EXPECT_EQ(kSbcFreq44100, c.freqCode);
    EXPECT_EQ(kSbcModeStereo, c.modeCode);
    EXPECT_EQ(0x22, c.freqMode);
}

TEST(A2dpParams, BitpoolIsClampedToTheDevicesRange)
{
    A2dpConfig c;
    ASSERT_TRUE(chooseA2dpParams(caps(0x11, 0x15, 2, 32), c));
    EXPECT_EQ(32u, c.bitpool); // the maximum is below 40
    ASSERT_TRUE(chooseA2dpParams(caps(0x11, 0x15, 45, 60), c));
    EXPECT_EQ(45u, c.bitpool); // the minimum is above 40
}

TEST(A2dpParams, BlockLengthFollowsWhatIsOffered)
{
    A2dpConfig c;
    ASSERT_TRUE(chooseA2dpParams(caps(0x11, 0x40 | 0x04 | 0x01, 2, 53), c)); // 8 blocks only
    EXPECT_EQ(1u, c.blocks);
    ASSERT_TRUE(chooseA2dpParams(caps(0x11, 0x80, 2, 53), c)); // 4 blocks only
    EXPECT_EQ(0u, c.blocks);
    EXPECT_EQ(0u, c.subbands8); // no 8 subbands offered
}

TEST(A2dpParams, RefusesWhatItCannotDo)
{
    bluev::testing::CaptureSink sink;
    A2dpConfig c;
    EXPECT_FALSE(chooseA2dpParams(caps(0x01, 0x15, 2, 53), c)); // neither 44.1 nor 48 kHz
    EXPECT_TRUE(sink.has("neither 44.1 nor 48 kHz"));
    EXPECT_FALSE(chooseA2dpParams(caps(0x10, 0x15, 2, 53), c)); // no stereo mode
    EXPECT_TRUE(sink.has("no stereo mode"));
}
