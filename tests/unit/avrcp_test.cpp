// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <vector>

#include "core/audio/avrcp.hpp"

using namespace bluev::avrcp;

TEST(Avrcp, RegisterVolumeIsANotifyCommandForTheVolumeEvent)
{
    std::uint8_t frame[32];
    const std::vector<std::uint8_t> expected = {0x50, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19, 0x58,
                                                0x31, 0x00, 0x00, 0x05, 0x0d, 0x00, 0x00, 0x00, 0x00};

    ASSERT_EQ(expected.size(), buildRegisterVolume(5, frame));
    EXPECT_EQ(expected, std::vector<std::uint8_t>(frame, frame + expected.size()));
}

TEST(Avrcp, TheLabelUsesOnlyFourBits)
{
    std::uint8_t frame[32];

    buildRegisterVolume(0x1a, frame);
    EXPECT_EQ(0xa0, frame[0]);
}

TEST(Avrcp, AnInterimAnswerCarriesTheVolume)
{
    // label 5, response; interim; pdu 0x31, event 0x0d, volume 0x40
    const std::uint8_t frame[] = {0x52, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19,
                                  0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x40};
    Message m;

    ASSERT_TRUE(parse(frame, sizeof(frame), m));
    EXPECT_EQ(5, m.label);
    EXPECT_TRUE(m.response);
    EXPECT_EQ(kInterim, m.ctype);
    EXPECT_EQ(0x31, m.pdu);
    EXPECT_EQ(0x40, volumeFromNotification(m));
}

TEST(Avrcp, TheTopBitOfTheVolumeByteIsIgnored)
{
    const std::uint8_t frame[] = {0x52, 0x11, 0x0e, 0x0d, 0x48, 0x00, 0x00, 0x19,
                                  0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0xff};
    Message m;

    ASSERT_TRUE(parse(frame, sizeof(frame), m));
    EXPECT_EQ(kChanged, m.ctype);
    EXPECT_EQ(127, volumeFromNotification(m));
}

TEST(Avrcp, ARejectionIsNoVolume)
{
    const std::uint8_t frame[] = {0x52, 0x11, 0x0e, 0x0a, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x01, 0x00};
    Message m;

    ASSERT_TRUE(parse(frame, sizeof(frame), m));
    EXPECT_EQ(kRejected, m.ctype);
    EXPECT_EQ(-1, volumeFromNotification(m));
}

TEST(Avrcp, ACommandIsNotAnAnswer)
{
    const std::uint8_t frame[] = {0x50, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19,
                                  0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x40};
    Message m;

    ASSERT_TRUE(parse(frame, sizeof(frame), m));
    EXPECT_FALSE(m.response);
    EXPECT_EQ(-1, volumeFromNotification(m));
}

TEST(Avrcp, OtherAvcCommandsParseWithoutAPdu)
{
    // UNIT INFO (opcode 0x30)
    const std::uint8_t frame[] = {0x10, 0x11, 0x0e, 0x01, 0x48, 0x30, 0xff, 0xff, 0xff, 0xff, 0xff};
    Message m;

    ASSERT_TRUE(parse(frame, sizeof(frame), m));
    EXPECT_EQ(0, m.pdu);
    EXPECT_FALSE(m.response);
}

TEST(Avrcp, ShortFragmentedAndForeignFramesDoNotParse)
{
    Message m;
    const std::uint8_t good[] = {0x52, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19,
                                 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x40};
    std::uint8_t bad[sizeof(good)];

    EXPECT_FALSE(parse(good, 5, m));

    std::copy(good, good + sizeof(good), bad);
    bad[0] |= 0x04; // start of a fragmented message
    EXPECT_FALSE(parse(bad, sizeof(bad), m));

    std::copy(good, good + sizeof(good), bad);
    bad[2] = 0x0c; // another profile
    EXPECT_FALSE(parse(bad, sizeof(bad), m));

    EXPECT_FALSE(parse(good, sizeof(good) - 1, m)); // the parameter length promises a byte that is not there
}

TEST(Avrcp, NotImplementedAnswersTheSameFrameAsAResponse)
{
    const std::uint8_t command[] = {0x70, 0x11, 0x0e, 0x01, 0x48, 0x7c, 0x44, 0x00};
    std::uint8_t answer[16];

    ASSERT_EQ(sizeof(command), buildNotImplemented(command, sizeof(command), answer));
    EXPECT_EQ(0x72, answer[0]); // same label, now a response
    EXPECT_EQ(kNotImplemented, answer[3]);
    EXPECT_EQ(0x7c, answer[5]);
}

TEST(Avrcp, SetVolumeIsAControlCommandWithOneByte)
{
    std::uint8_t frame[32];
    const std::vector<std::uint8_t> expected = {0x30, 0x11, 0x0e, 0x00, 0x48, 0x00, 0x00,
                                                0x19, 0x58, 0x50, 0x00, 0x00, 0x01, 0x40};

    ASSERT_EQ(expected.size(), buildSetVolume(3, 0x40, frame));
    EXPECT_EQ(expected, std::vector<std::uint8_t>(frame, frame + expected.size()));
    buildSetVolume(3, 0xff, frame);
    EXPECT_EQ(0x7f, frame[13]); // only 7 bits
}

TEST(Avrcp, TheAnswerToSetVolumeCarriesTheVolumeTheHeadsetTook)
{
    const std::uint8_t accepted[] = {0x32, 0x11, 0x0e, 0x09, 0x48, 0x00, 0x00,
                                     0x19, 0x58, 0x50, 0x00, 0x00, 0x01, 0x45};
    std::uint8_t rejected[sizeof(accepted)];
    Message m;

    ASSERT_TRUE(parse(accepted, sizeof(accepted), m));
    EXPECT_EQ(0x45, volumeFromSetAnswer(m));

    std::copy(accepted, accepted + sizeof(accepted), rejected);
    rejected[3] = 0x0a;
    ASSERT_TRUE(parse(rejected, sizeof(rejected), m));
    EXPECT_EQ(-1, volumeFromSetAnswer(m));
}
