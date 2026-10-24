// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "core/bluetooth/address.hpp"

using namespace bluev;

TEST(Address, TextIsMostSignificantByteFirst)
{
    const BdAddr a = {0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}; // HCI order
    EXPECT_EQ("00:11:22:AA:BB:CC", addressString(a));
    EXPECT_STREQ("00:11:22:AA:BB:CC", addressText(a).c_str());
}

TEST(Address, ParseRoundTrips)
{
    BdAddr a{};
    ASSERT_TRUE(parseAddress("00:11:22:aa:bb:cc", a));
    EXPECT_EQ((BdAddr{0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00}), a);
    EXPECT_EQ("00:11:22:AA:BB:CC", addressString(a));
}

TEST(Address, ParseRejectsShortAndWrongText)
{
    BdAddr a{};
    EXPECT_FALSE(parseAddress("", a));
    EXPECT_FALSE(parseAddress("zz:zz", a));
    EXPECT_FALSE(parseAddress("00:11:22:AA:BB", a));
    EXPECT_FALSE(parseAddress("00:11:22:AA:BB:CC extra", a)) << "text after the address";
    EXPECT_FALSE(parseAddress("00:11:22:AA:BB:CC:DD", a)) << "a seventh pair";
    EXPECT_FALSE(parseAddress("not an address at all, much too long for one", a));
}

TEST(Address, AddressFromCopiesSixBytes)
{
    const std::uint8_t raw[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ((BdAddr{1, 2, 3, 4, 5, 6}), addressFrom(raw));
}
