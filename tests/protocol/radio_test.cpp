// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "capture_sink.hpp"
#include "core/radio.hpp"
#include "fake_audio.hpp"
#include "fake_speaker.hpp"
#include "fake_stores.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

class RadioTest : public ::testing::Test
{
  protected:
    RadioTest()
        : controller(clock), speaker(controller, clock), audio(clock), radio(controller, bonds, usb, audio, clock)
    {
        usb.layouts["ugen1.2"] = HciLayout{0, 0x81, 0x82, 0x02};
        bonds.put(controller.localAddress, controller.remote.address, 0x5a, "Creative");
        speaker.attach();
    }

    CaptureSink sink;
    FakeClock clock;
    FakeController controller;
    FakeSpeaker speaker;
    FakeAudio audio;
    FakeBondStore bonds;
    FakeUsbDirectory usb;
    Radio radio;
};

} // namespace

TEST_F(RadioTest, OpenStartsTheControllerInServiceMode)
{
    EXPECT_TRUE(radio.open("ugen1.2"));
    EXPECT_EQ("ugen1.2", controller.selectedNode);
    EXPECT_EQ("", radio.openError());
    // service mode: an unknown device dialing in is counted, not accepted
    controller.event(0x04, {0x99, 0, 0, 0, 0, 0, 0x04, 0x04, 0x24, 1});
    radio.poll(10);
    EXPECT_EQ(1, radio.foreignRequests());
}

TEST_F(RadioTest, OpenFailureCarriesTheReason)
{
    controller.openOk = false;
    EXPECT_FALSE(radio.open("ugen1.2"));
    EXPECT_EQ("the adapter could not be opened", radio.openError());
}

TEST_F(RadioTest, CloseDropsTheTickAndStopsTheStack)
{
    ASSERT_TRUE(radio.open("ugen1.2"));
    int ticks = 0;
    radio.setTick([&] { ticks++; });
    for (int i = 0; i < 30; i++)
    {
        radio.poll(100);
    }
    EXPECT_GE(ticks, 2);
    const int before = ticks;
    radio.close();
    EXPECT_EQ(1, controller.closes);
    radio.poll(1500);
    EXPECT_EQ(before, ticks);
}

TEST_F(RadioTest, ScanListsDevicesWithTheJunkFlag)
{
    InquiryDevice tv, speakerDevice;
    tv.address = BdAddr{{1, 1, 1, 1, 1, 1}};
    tv.classOfDevice = 0x24042c;
    tv.name = "Samsung TV";
    speakerDevice.address = controller.remote.address;
    speakerDevice.name = "Creative";
    controller.inquiryDevices = {tv, speakerDevice};
    ASSERT_TRUE(radio.open("ugen1.2"));

    EXPECT_EQ(2, radio.scan(10000));
    const auto devices = radio.devices();
    ASSERT_EQ(2u, devices.size());
    EXPECT_EQ("Samsung TV", devices[0].name);
    EXPECT_TRUE(devices[0].other);
    EXPECT_FALSE(devices[1].other);
    EXPECT_EQ(-50, devices[1].rssi);
    EXPECT_TRUE(radio.hasBond(devices[1].address));
    ASSERT_EQ(1u, radio.bonds().size());
    radio.forget(devices[1].address);
    EXPECT_FALSE(radio.hasBond(devices[1].address));
}

TEST_F(RadioTest, ASessionFromConnectToTheEnd)
{
    ASSERT_TRUE(radio.open("ugen1.2"));
    radio.resetAudio();
    ASSERT_TRUE(radio.connectTo(controller.remote.address)) << sink.all();
    EXPECT_EQ("Creative", radio.peerName());
    EXPECT_EQ("00:11:22:AA:BB:CC", radio.peerAddress());
    ASSERT_TRUE(radio.startAudio(true)) << sink.all();

    RecordingObserver observer;
    radio.setStreamObserver(&observer);
    controller.remoteDisconnect(0x13, 3000);
    EXPECT_TRUE(radio.stream(true));
    EXPECT_GT(speaker.media.size(), 100u);
    EXPECT_GT(observer.count, 5);

    radio.stopAudio(); // does nothing on a lost link
    radio.disconnect();
    radio.resetAudio();
    radio.listenAudio();
    EXPECT_FALSE(radio.linkUp());
}

TEST_F(RadioTest, RequestStopEndsTheStreamWithoutLosingTheLink)
{
    ASSERT_TRUE(radio.open("ugen1.2"));
    ASSERT_TRUE(radio.connectTo(controller.remote.address));
    ASSERT_TRUE(radio.startAudio(true));
    audio.onRead = [&] {
        if (clock.nowMs() > 100000 + 20000)
        {
            radio.requestStop();
        }
    };
    EXPECT_TRUE(radio.stream(true));
    EXPECT_TRUE(radio.linkUp());
    radio.stopAudio();
    EXPECT_TRUE(speaker.closed);
}

TEST_F(RadioTest, APairedDeviceThatDialsInIsAdopted)
{
    ASSERT_TRUE(radio.open("ugen1.2"));
    controller.event(0x04, {0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00, 0x04, 0x04, 0x24, 1});
    ASSERT_TRUE(radio.linkUp() || [&] {
        for (int i = 0; i < 50 && !radio.linkUp(); i++)
        {
            radio.poll(100);
        }
        return radio.linkUp();
    }());
    EXPECT_TRUE(radio.adoptIncoming()) << sink.all();
    speaker.dialIn(100);
    EXPECT_TRUE(radio.startAudio(false)) << sink.all();
}
