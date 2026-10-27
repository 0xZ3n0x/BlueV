// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/bluetooth/realtek_firmware.hpp"
#include "realtek_image.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

const std::vector<std::uint8_t> kNoConfig;

ChipIdentity romIdentity()
{
    ChipIdentity id;

    id.manufacturer = 0x005d;
    id.hciVersion = 0x0a;
    id.hciRevision = 0x000b;
    id.lmpVersion = 0x0a;
    id.lmpSubversion = 0x8761;
    return id;
}

} // namespace

TEST(RealtekFirmware, V1PicksThePatchForTheRomVersionAndStampsTheVersion)
{
    const std::vector<std::uint8_t> config = {0x51, 0xaa, 0x00, 0x00};
    std::vector<std::uint8_t> image;
    std::string why;

    ASSERT_TRUE(realtekImage(realtekV1Image(1), config, 1, image, why)) << why;
    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3, 4, 0x44, 0x33, 0x22, 0x11, 0x51, 0xaa, 0x00, 0x00}), image);
}

TEST(RealtekFirmware, V1RefusesAnUnknownRomVersion)
{
    std::vector<std::uint8_t> image;
    std::string why;

    EXPECT_FALSE(realtekImage(realtekV1Image(1), kNoConfig, 7, image, why));
    EXPECT_NE(std::string::npos, why.find("no patch for ROM version 7"));
}

TEST(RealtekFirmware, V1RefusesACutOffFile)
{
    std::vector<std::uint8_t> firmware = realtekV1Image(1);
    std::vector<std::uint8_t> image;
    std::string why;

    firmware.resize(20);
    EXPECT_FALSE(realtekImage(firmware, kNoConfig, 1, image, why));
    EXPECT_FALSE(why.empty());
}

TEST(RealtekFirmware, V2OrdersTheSubsectionsAndKeepsOnlyThisRomVersion)
{
    std::vector<std::uint8_t> image;
    std::string why;

    ASSERT_TRUE(realtekImage(realtekV2Image(1), kNoConfig, 1, image, why)) << why;
    EXPECT_EQ((std::vector<std::uint8_t>{0x11, 0x22, 0x33, 0xaa, 0xbb, 0xcc, 0xdd}), image);
}

TEST(RealtekFirmware, V2RefusesAnUnknownRomVersion)
{
    std::vector<std::uint8_t> image;
    std::string why;

    EXPECT_FALSE(realtekImage(realtekV2Image(1), kNoConfig, 5, image, why));
    EXPECT_NE(std::string::npos, why.find("no patch for ROM version 5"));
}

TEST(RealtekFirmware, RefusesAFileThatIsNotFirmware)
{
    const std::vector<std::uint8_t> firmware(64, 0x5a);
    std::vector<std::uint8_t> image;
    std::string why;

    EXPECT_FALSE(realtekImage(firmware, kNoConfig, 1, image, why));
    EXPECT_NE(std::string::npos, why.find("not a Realtek firmware file"));
}

TEST(RealtekFirmware, FragmentsAreTheIndexByteAndAtMost252Bytes)
{
    const std::vector<std::uint8_t> image(600, 0x5a);
    const auto fragments = realtekFragments(image);

    ASSERT_EQ(3u, fragments.size());
    EXPECT_EQ(253u, fragments[0].size());
    EXPECT_EQ(0, fragments[0][0]);
    EXPECT_EQ(1, fragments[1][0]);
    EXPECT_EQ(0x82, fragments[2][0]);    // last fragment of three
    EXPECT_EQ(97u, fragments[2].size()); // index + 96 bytes
    EXPECT_EQ(0x5a, fragments[2][1]);
}

TEST(RealtekFirmware, AFragmentExactly252BytesLongIsFollowedByAnEmptyLastOne)
{
    const std::vector<std::uint8_t> image(504, 0x11);
    const auto fragments = realtekFragments(image);

    ASSERT_EQ(3u, fragments.size());
    EXPECT_EQ(0x82, fragments[2][0]);
    EXPECT_EQ(1u, fragments[2].size());
}

TEST(RealtekFirmware, TheIndexSkipsTheEndMarker)
{
    const std::vector<std::uint8_t> image(252 * 128, 0x22);
    const auto fragments = realtekFragments(image);

    ASSERT_EQ(129u, fragments.size());
    EXPECT_EQ(0x7f, fragments[127][0]);
    EXPECT_EQ(0x81, fragments[128][0]); // after 0x7f the count restarts at 1
    EXPECT_EQ(1u, fragments[128].size());
}

TEST(RealtekFirmware, TheChipTableMatchesOnlyRomMode)
{
    const ChipIdentity id = romIdentity();
    const RealtekChip* chip = realtekChipFor(id);

    ASSERT_NE(nullptr, chip);
    EXPECT_STREQ("RTL8761BU", chip->name);
    EXPECT_STREQ("rtl8761bu_fw.bin", chip->firmwareFile);

    // after the upload the revision and subversion change
    ChipIdentity loaded = id;
    loaded.lmpSubversion = 0xdfc6;
    loaded.hciRevision = 0xd922;
    EXPECT_EQ(nullptr, realtekChipFor(loaded));
    EXPECT_TRUE(realtekInRomMode(id));
    EXPECT_FALSE(realtekInRomMode(loaded));
}
