// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "core/bluetooth/names.hpp"

using namespace bluev;

TEST(Names, HciStatus)
{
    EXPECT_STREQ("success", hciStatusName(0x00));
    EXPECT_STREQ("page timeout, the device did not answer", hciStatusName(0x04));
    EXPECT_STREQ("the device ended the connection", hciStatusName(0x13));
    EXPECT_STREQ("ended by us", hciStatusName(0x16));
    EXPECT_STREQ("unknown reason", hciStatusName(0xee));
}

TEST(Names, Opcodes)
{
    EXPECT_STREQ("Reset", hciOpcodeName(0x0c03));
    EXPECT_STREQ("Write Link Policy Settings", hciOpcodeName(0x080d));
    EXPECT_STREQ("Read BD_ADDR", hciOpcodeName(0x1009));
    EXPECT_STREQ("command", hciOpcodeName(0xffff));
}

TEST(Names, DeviceClasses)
{
    EXPECT_STREQ("audio/video", deviceClassName(0x240404));
    EXPECT_STREQ("gamepad", deviceClassName(0x002508));
    EXPECT_STREQ("computer", deviceClassName(0x000104));
    EXPECT_STREQ("phone", deviceClassName(0x5a020c));
    EXPECT_STREQ("other", deviceClassName(0x001f00));
}

TEST(Names, ControllerMakersAndVersions)
{
    EXPECT_STREQ("MediaTek", manufacturerName(0x0046));
    EXPECT_STREQ("CSR", manufacturerName(0x000a));
    EXPECT_STREQ("unknown", manufacturerName(0x7777));
    EXPECT_STREQ("5.2", hciVersionName(11));
    EXPECT_STREQ("4.0", hciVersionName(6));
    EXPECT_STREQ("newer than 5.4", hciVersionName(99));
}
