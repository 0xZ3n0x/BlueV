// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <vector>

#include "core/audio/avdtp.hpp"

using namespace bluev::avdtp;

TEST(Avdtp, ParseHeader)
{
    Header h;
    const std::uint8_t cmd[] = {0x50, 0x01}; // label 5, command, discover
    ASSERT_TRUE(parseHeader(cmd, 2, h));
    EXPECT_EQ(5, h.label);
    EXPECT_EQ(kCommand, h.type);
    EXPECT_EQ(kDiscover, h.signal);

    const std::uint8_t accept[] = {0x32, 0x43}; // label 3, accept, signal 3 (high bits masked)
    ASSERT_TRUE(parseHeader(accept, 2, h));
    EXPECT_EQ(3, h.label);
    EXPECT_EQ(kAccept, h.type);
    EXPECT_EQ(kSetConfiguration, h.signal);
}

TEST(Avdtp, ParseHeaderRejectsShortAndFragmentedFrames)
{
    Header h;
    const std::uint8_t one[] = {0x50};
    EXPECT_FALSE(parseHeader(one, 1, h));
    const std::uint8_t fragment[] = {0x54, 0x01}; // packet type: start of a fragmented message
    EXPECT_FALSE(parseHeader(fragment, 2, h));
}

TEST(Avdtp, BuildMessages)
{
    std::uint8_t out[16];
    const std::uint8_t sep = 0x04;

    ASSERT_EQ(3u, buildCommand(2, kOpen, &sep, 1, out));
    EXPECT_EQ(0x20, out[0]);
    EXPECT_EQ(kOpen, out[1]);
    EXPECT_EQ(0x04, out[2]);

    ASSERT_EQ(2u, buildAccept(0xa, kStart, out));
    EXPECT_EQ(0xa2, out[0]);
    EXPECT_EQ(kStart, out[1]);

    ASSERT_EQ(3u, buildReject(1, 9, 0x19, out));
    EXPECT_EQ(0x13, out[0]);
    EXPECT_EQ(9, out[1]);
    EXPECT_EQ(0x19, out[2]);

    ASSERT_EQ(4u, buildDiscoverAccept(7, out));
    EXPECT_EQ(0x72, out[0]);
    EXPECT_EQ(kDiscover, out[1]);
    EXPECT_EQ(0x04, out[2]); // seid 1 not in use
    EXPECT_EQ(0x00, out[3]); // audio, source

    ASSERT_EQ(12u, buildCapabilitiesAccept(7, kGetCapabilities, out));
    EXPECT_EQ(0x72, out[0]);
    EXPECT_EQ(kGetCapabilities, out[1]);
    EXPECT_EQ(0x07, out[4]);  // media codec capability
    EXPECT_EQ(0x35, out[11]); // maximum bitpool 53

    // Get All Capabilities (AVDTP 1.3) gets the same list under its own signal id
    ASSERT_EQ(12u, buildCapabilitiesAccept(7, kGetAllCapabilities, out));
    EXPECT_EQ(0x0c, out[1]);
    EXPECT_EQ(0x35, out[11]);
}

TEST(Avdtp, ParseSetConfiguration)
{
    // acceptor seid, initiator seid, media transport, media codec (SBC, 44.1 kHz stereo, bitpool 2..53)
    const std::vector<std::uint8_t> frame = {0x30, 0x03, 0x08, 0x04, 0x01, 0x00, 0x07,
                                             0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35};
    PeerConfig c;
    ASSERT_TRUE(parseSetConfiguration(frame.data(), static_cast<int>(frame.size()), c));
    EXPECT_EQ(0x04, c.seid); // the device's own endpoint: the fourth byte, kept as sent (1 << 2)
    ASSERT_TRUE(c.haveSbc);
    EXPECT_EQ(0x21, c.caps & 0xff); // 44.1 kHz | joint stereo
    EXPECT_EQ(0x15, (c.caps >> 8) & 0xff);
    EXPECT_EQ(2, (c.caps >> 16) & 0xff);
    EXPECT_EQ(53, (c.caps >> 24) & 0xff);
}

TEST(Avdtp, SetConfigurationWithoutSbcOrTooShort)
{
    PeerConfig c;
    const std::vector<std::uint8_t> noSbc = {0x30, 0x03, 0x08, 0x04, 0x01, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_TRUE(parseSetConfiguration(noSbc.data(), static_cast<int>(noSbc.size()), c));
    EXPECT_FALSE(c.haveSbc);
    const std::vector<std::uint8_t> shortFrame = {0x30, 0x03, 0x08, 0x04};
    EXPECT_FALSE(parseSetConfiguration(shortFrame.data(), 4, c));
}

TEST(Avdtp, ParseEndpoints)
{
    // header, then (seid << 2 | in use, type << 3) pairs: seid 1 source, seid 2 sink
    const std::vector<std::uint8_t> rsp = {0x12, 0x01, 0x04, 0x00, 0x08, 0x08};
    Endpoint e[4];
    ASSERT_EQ(2, parseEndpoints(rsp.data(), static_cast<int>(rsp.size()), e, 4));
    EXPECT_EQ(1, e[0].seid);
    EXPECT_FALSE(e[0].isSink);
    EXPECT_EQ(2, e[1].seid);
    EXPECT_TRUE(e[1].isSink);
    EXPECT_EQ(1, parseEndpoints(rsp.data(), static_cast<int>(rsp.size()), e, 1));
}

TEST(Avdtp, FindSbcCapabilities)
{
    // media transport, then media codec: audio, SBC, 48 kHz joint stereo, 16 blocks 8 subbands loudness, 2..53
    const std::vector<std::uint8_t> rsp = {0x22, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 0x02, 0x35};
    std::uint32_t caps = 0;
    ASSERT_TRUE(findSbcCapabilities(rsp.data(), static_cast<int>(rsp.size()), caps));
    EXPECT_EQ(0x35021511u, caps);

    const std::vector<std::uint8_t> none = {0x22, 0x02, 0x01, 0x00};
    EXPECT_FALSE(findSbcCapabilities(none.data(), 4, caps));
    EXPECT_FALSE(findSbcCapabilities(none.data(), 2, caps)); // too short to look at
}
