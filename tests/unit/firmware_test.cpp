// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <vector>

#include "core/bluetooth/firmware.hpp"
#include "realtek_image.hpp"

using namespace bluev;
using namespace bluev::testing;

TEST(Firmware, ReadsTheIdentityFromTheLocalVersionReply)
{
    // 0e len ncmd 01 10 status hci_ver hci_rev(2) lmp_ver maker(2) lmp_sub(2)
    const std::vector<std::uint8_t> event = {0x0e, 0x0c, 1,    0x01, 0x10, 0,    0x0a,
                                             0x0b, 0x00, 0x0a, 0x5d, 0x00, 0x61, 0x87};
    ChipIdentity id;

    ASSERT_TRUE(chipIdentityFromLocalVersion(event.data(), static_cast<std::uint32_t>(event.size()), id));
    EXPECT_EQ(0x005d, id.manufacturer);
    EXPECT_EQ(0x0a, id.hciVersion);
    EXPECT_EQ(0x000b, id.hciRevision);
    EXPECT_EQ(0x8761, id.lmpSubversion);

    EXPECT_FALSE(chipIdentityFromLocalVersion(event.data(), 13, id));
    std::vector<std::uint8_t> other = event;
    other[3] = 0x02; // not Read Local Version
    EXPECT_FALSE(chipIdentityFromLocalVersion(other.data(), static_cast<std::uint32_t>(other.size()), id));
}

TEST(Firmware, NamesTheFamilies)
{
    ChipIdentity id;

    id.manufacturer = 0x000a;
    EXPECT_EQ(FirmwareFamily::Csr, firmwareFamily(id));
    id.manufacturer = 0x005d;
    EXPECT_EQ(FirmwareFamily::Realtek, firmwareFamily(id));
    id.manufacturer = 0x000f;
    EXPECT_EQ(FirmwareFamily::Broadcom, firmwareFamily(id));
    id.manufacturer = 0x0002;
    EXPECT_EQ(FirmwareFamily::Intel, firmwareFamily(id));
    id.manufacturer = 0x0046;
    EXPECT_EQ(FirmwareFamily::MediaTek, firmwareFamily(id));
    id.manufacturer = 0x1234;
    EXPECT_EQ(FirmwareFamily::Unknown, firmwareFamily(id));

    EXPECT_STREQ("Realtek", firmwareFamilyName(FirmwareFamily::Realtek));
    EXPECT_STREQ("unknown", firmwareFamilyName(FirmwareFamily::Unknown));
    EXPECT_TRUE(firmwareUploadSupported(FirmwareFamily::Realtek));
    EXPECT_FALSE(firmwareUploadSupported(FirmwareFamily::Broadcom));
}
