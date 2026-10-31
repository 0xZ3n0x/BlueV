// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_fixture.hpp"
#include "core/bluetooth/hci_defs.hpp"

using namespace bluev;
using namespace bluev::testing;
using namespace bluev::hci;

namespace
{

InquiryDevice device(std::uint8_t last, const char* name, std::uint32_t cod = 0x240404)
{
    InquiryDevice d;
    d.address = {{last, 0xBB, 0xAA, 0x22, 0x11, 0x00}};
    d.name = name;
    d.classOfDevice = cod;
    return d;
}

} // namespace

TEST_F(BtFixture, ScanFindsDevicesAndStopsWhenTheInquiryEnds)
{
    controller.inquiryDevices = {device(0x41, "Creative Pebble"), device(0x42, "Sony SRS")};
    startExternal();

    const auto t0 = clock.nowMs();
    EXPECT_EQ(2, stack.scan(10000));
    EXPECT_LT(clock.nowMs() - t0, 4000u); // ended with the inquiry, not at the timeout
    ASSERT_EQ(2u, stack.devices().size());
    EXPECT_EQ("Creative Pebble", stack.devices()[0].name);
    EXPECT_EQ(-50, stack.devices()[0].rssi);
    EXPECT_EQ(0x240404u, stack.devices()[0].classOfDevice);
    EXPECT_TRUE(sink.has("scan: 10 s"));
    EXPECT_TRUE(sink.has("scan: 2 device(s)"));
    EXPECT_TRUE(sink.has("found 00:11:22:AA:BB:41 'Creative Pebble' ("));
    EXPECT_FALSE(controller.sawCommand(kInquiryCancel));
}

TEST_F(BtFixture, ScanIsCancelledAtTheTimeout)
{
    controller.inquiryDurationMs = 60000;
    controller.inquiryDevices = {device(0x41, "Creative")};
    startExternal();
    EXPECT_EQ(1, stack.scan(2000));
    EXPECT_TRUE(controller.sawCommand(kInquiryCancel));
}

TEST_F(BtFixture, TheConsolesChipPausesOnlyItsInquiryScanForAScan)
{
    controller.inquiryDevices = {device(0x41, "Creative")};
    controller.scanSetting = 3;
    startInternal();
    stack.scan(10000);

    std::vector<int> written;
    for (const SentCommand& c : controller.commands)
    {
        if (c.opcode == kWriteScanEnable)
        {
            written.push_back(c.params[0]);
        }
    }
    EXPECT_EQ((std::vector<int>{2, 3}), written); // page scan stays on throughout
}

TEST_F(BtFixture, AScanOnAnAdapterDoesNotTouchItsScanSetting)
{
    controller.inquiryDevices = {device(0x41, "Creative")};
    startExternal();
    stack.scan(10000);
    EXPECT_FALSE(controller.sawCommand(kWriteScanEnable));
}

TEST_F(BtFixture, ANewScanStartsFromAnEmptyList)
{
    controller.inquiryDevices = {device(0x41, "Creative")};
    startExternal();
    stack.scan(10000);
    controller.inquiryDevices = {device(0x42, "Sony")};
    EXPECT_EQ(1, stack.scan(10000));
    EXPECT_EQ("Sony", stack.devices()[0].name);
}

TEST_F(BtFixture, TelevisionsAreMarked)
{
    controller.inquiryDevices = {device(0x41, "Samsung TV", 0x24042c), device(0x42, "Soundbar X"),
                                 device(0x43, "Creative")};
    startExternal();
    stack.scan(10000);

    EXPECT_TRUE(sink.has("'Samsung TV' ("));
    EXPECT_EQ(2, sink.count("[ignored]")); // the TV by class, the soundbar by name
}

TEST_F(BtFixture, DevicesThatAreNotAudioAreSkippedAndLoggedFewTimes)
{
    controller.inquiryDurationMs = 20000;
    for (int i = 0; i < 60; i++)
    {
        controller.inquiryDevices.push_back(device(static_cast<std::uint8_t>(i), "Phone", 0x5a020c));
    }
    controller.inquiryDevices.push_back(device(0xe0, "Creative"));
    startExternal();
    EXPECT_EQ(1, stack.scan(30000));
    EXPECT_EQ(40, sink.count("not audio/video"));
}

TEST_F(BtFixture, AtMostSixteenDevicesAreKept)
{
    controller.inquiryDurationMs = 20000;
    for (int i = 0; i < 20; i++)
    {
        controller.inquiryDevices.push_back(device(static_cast<std::uint8_t>(i + 1), "Speaker"));
    }
    startExternal();
    EXPECT_EQ(16, stack.scan(30000));
}

TEST_F(BtFixture, ANameThatArrivesLaterUpdatesTheEntry)
{
    InquiryDevice d = device(0x41, "");
    controller.inquiryDevices = {d};
    startExternal();
    stack.scan(10000);
    ASSERT_EQ(1u, stack.devices().size());
    EXPECT_TRUE(stack.devices()[0].name.empty());

    // the same device again, now with its name (extended inquiry result)
    Bytes r = {1,    0x41, 0xBB, 0xAA, 0x22, 0x11, 0x00, 1,   0,   0x04, 0x04,
               0x24, 0x34, 0x12, 0xc4, 5,    0x09, 'S',  'o', 'n', 'y'};
    controller.event(0x2f, r);
    stack.poll(10);
    ASSERT_EQ(1u, stack.devices().size());
    EXPECT_EQ("Sony", stack.devices()[0].name);
    EXPECT_EQ(-60, stack.devices()[0].rssi);
}

TEST_F(BtFixture, BondsAreListedPerAdapterAndForgotten)
{
    const BdAddr a{{1, 2, 3, 4, 5, 6}}, b{{6, 5, 4, 3, 2, 1}};
    bonds.put(controller.localAddress, a, 1, "Creative");
    bonds.put(BdAddr{{9, 9, 9, 9, 9, 9}}, b, 2, "Sony"); // another adapter's
    startExternal();

    EXPECT_TRUE(stack.hasBond(a));
    EXPECT_FALSE(stack.hasBond(b));
    ASSERT_EQ(1u, stack.bonds().size());
    EXPECT_EQ("Creative", stack.bonds()[0].name);

    stack.forget(a);
    EXPECT_FALSE(stack.hasBond(a));
    EXPECT_TRUE(sink.has("forgot 06:05:04:03:02:01"));
}

TEST_F(BtFixture, NoBondsAreVisibleBeforeTheAdapterIsKnown)
{
    bonds.put(controller.localAddress, BdAddr{{1, 2, 3, 4, 5, 6}}, 1, "Creative");
    EXPECT_FALSE(stack.hasBond(BdAddr{{1, 2, 3, 4, 5, 6}}));
    EXPECT_TRUE(stack.bonds().empty());
}
