// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include "capture_sink.hpp"
#include "core/bluetooth/usb_hci_transport.hpp"
#include "fake_clock.hpp"
#include "fake_usb_device.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

constexpr const char* kConsoleNode = "ugen0.2";
constexpr int kFirstAcl = 48;
constexpr int kAclOut = 64;

class UsbTransport : public ::testing::Test
{
  protected:
    UsbTransport() : device(clock), transport(device, clock, kConsoleNode)
    {
    }

    // the standard layout: events 0x81, ACL in 0x82, ACL out 0x02
    void openStandard()
    {
        transport.select("ugen1.2", HciLayout{0, 0x81, 0x82, 0x02});
        ASSERT_TRUE(transport.open()) << sink.all();
    }

    static UsbBytes event(std::uint8_t code, std::uint8_t length)
    {
        UsbBytes packet = {code, length};
        packet.insert(packet.end(), length, 0xaa);
        return packet;
    }

    static UsbBytes aclPacket(std::uint16_t handle, std::uint16_t length)
    {
        UsbBytes packet = {static_cast<std::uint8_t>(handle), static_cast<std::uint8_t>(handle >> 8),
                           static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8)};
        packet.insert(packet.end(), length, 0xbb);
        return packet;
    }

    UsbBytes nextEvent()
    {
        std::uint8_t out[kUsbTransferBytes];
        const int n = transport.nextEvent(out, sizeof out);

        return UsbBytes(out, out + n);
    }

    UsbBytes nextAcl()
    {
        std::uint8_t out[kUsbTransferBytes];
        const int n = transport.nextAcl(out, sizeof out);

        return UsbBytes(out, out + n);
    }

    CaptureSink sink;
    FakeClock clock;
    FakeUsbDevice device;
    UsbHciTransport transport;
};

} // namespace

// ---- opening ----

TEST_F(UsbTransport, OpensTheTransferTableWithTheEndpointsOfTheLayout)
{
    openStandard();

    EXPECT_EQ("ugen1.2", device.node);
    EXPECT_EQ(kUsbTransfers, device.transferCount);
    EXPECT_EQ(kAclOut, device.forceShortIndex);
    ASSERT_EQ(1u + 48u + 16u, device.openedTransfers.size());
    EXPECT_EQ(kAclOut, device.openedTransfers[0].index); // bulk out first
    EXPECT_EQ(0x02, device.openedTransfers[0].endpoint);
    EXPECT_EQ(0, device.openedTransfers[1].index);
    EXPECT_EQ(0x81, device.openedTransfers[1].endpoint);
    EXPECT_EQ(static_cast<std::uint32_t>(kUsbTransferBytes), device.openedTransfers[1].length);
    EXPECT_EQ(47, device.openedTransfers[48].index);
    EXPECT_EQ(kFirstAcl, device.openedTransfers[49].index);
    EXPECT_EQ(0x82, device.openedTransfers[49].endpoint);
    EXPECT_TRUE(sink.has("hci: controller open alongside the system, 48 event and 16 ACL reads"));
    EXPECT_TRUE(device.starts.empty()); // nothing runs until the first pump
}

TEST_F(UsbTransport, TheConsolesOwnChipIsTheDefaultNode)
{
    transport.select(nullptr, HciLayout{});
    ASSERT_TRUE(transport.open());
    EXPECT_EQ(kConsoleNode, device.node);
}

TEST_F(UsbTransport, AnInterruptEventPipeIsReadWithASmallerBuffer)
{
    transport.select("ugen1.2", HciLayout{0, 0x82, 0x81, 0x01});
    ASSERT_TRUE(transport.open());
    EXPECT_EQ(0x104u, device.openedTransfers[1].length); // the events
    EXPECT_EQ(static_cast<std::uint32_t>(kUsbTransferBytes), device.openedTransfers[49].length);
}

TEST_F(UsbTransport, ADeviceThatCannotBeOpenedFailsWithoutLeavingAnythingOpen)
{
    device.failOpen = true;
    transport.select("ugen1.2", HciLayout{});

    EXPECT_FALSE(transport.open());
    EXPECT_TRUE(device.openedTransfers.empty());
    EXPECT_FALSE(device.opened);
}

TEST_F(UsbTransport, WithoutTheOutTransferTheDeviceIsClosedAgain)
{
    device.failOpenTransferAt = {kAclOut};
    transport.select("ugen1.2", HciLayout{});

    EXPECT_FALSE(transport.open());
    EXPECT_TRUE(device.closed);
    EXPECT_TRUE(sink.has("hci: USB_FS_OPEN 0x2 (transfer 64) errno=5"));
}

TEST_F(UsbTransport, WithoutAnyAclReadTheDeviceIsClosedAgain)
{
    device.failOpenTransferAt = {kFirstAcl};
    transport.select("ugen1.2", HciLayout{0, 0x81, 0x82, 0x02});

    EXPECT_FALSE(transport.open());
    EXPECT_TRUE(device.closed);
}

TEST_F(UsbTransport, WithoutAnyEventReadTheDeviceIsClosedAgain)
{
    device.failOpenTransferAt = {0};
    transport.select("ugen1.2", HciLayout{0, 0x81, 0x82, 0x02});

    EXPECT_FALSE(transport.open());
    EXPECT_TRUE(device.closed);
}

TEST_F(UsbTransport, FewerReadsThanWantedAreEnoughToGoOn)
{
    device.failOpenTransferAt = {40, 53}; // the 41st event read and the 6th ACL read
    transport.select("ugen1.2", HciLayout{0, 0x81, 0x82, 0x02});

    ASSERT_TRUE(transport.open());
    EXPECT_TRUE(sink.has("40 event and 5 ACL reads"));
    transport.pump(0);
    EXPECT_EQ(1, device.startCount(39));
    EXPECT_EQ(0, device.startCount(40)); // never opened, never started
    EXPECT_EQ(1, device.startCount(52));
    EXPECT_EQ(0, device.startCount(53));
}

// ---- reading ----

TEST_F(UsbTransport, ThePumpStartsEveryReadOnceAndNotAgainWhileTheyRun)
{
    openStandard();

    EXPECT_FALSE(transport.pump(0));
    EXPECT_EQ(48 + 16, static_cast<int>(device.starts.size()));
    EXPECT_EQ(static_cast<std::uint32_t>(kUsbTransferBytes), device.starts[0].second);
    EXPECT_FALSE(transport.pump(0));
    EXPECT_EQ(48 + 16, static_cast<int>(device.starts.size())); // still the same ones
}

TEST_F(UsbTransport, ACompletedEventReadComesOutAndTheReadIsStartedAgain)
{
    openStandard();
    transport.pump(0);

    ASSERT_TRUE(device.deliver(7, event(0x0e, 4)));
    EXPECT_TRUE(transport.pump(0));
    EXPECT_EQ(event(0x0e, 4), nextEvent());
    EXPECT_EQ(UsbBytes(), nextEvent());
    EXPECT_EQ(1, device.startCount(7)); // started again by the next pump, not before
    transport.pump(0);
    EXPECT_EQ(2, device.startCount(7));
}

TEST_F(UsbTransport, SeveralPacketsInOneReadAreSplit)
{
    openStandard();
    transport.pump(0);

    UsbBytes both = event(0x05, 4);
    const UsbBytes second = event(0x13, 5);

    both.insert(both.end(), second.begin(), second.end());
    ASSERT_TRUE(device.deliver(0, both));
    EXPECT_TRUE(transport.pump(0));
    EXPECT_EQ(event(0x05, 4), nextEvent());
    EXPECT_EQ(event(0x13, 5), nextEvent());
}

TEST_F(UsbTransport, AclReadsGoToTheAclQueue)
{
    openStandard();
    transport.pump(0);

    ASSERT_TRUE(device.deliver(kFirstAcl + 3, aclPacket(0x2042, 20)));
    EXPECT_TRUE(transport.pump(0));
    EXPECT_EQ(UsbBytes(), nextEvent());
    EXPECT_EQ(aclPacket(0x2042, 20), nextAcl());
}

TEST_F(UsbTransport, ThePumpWaitsForTheTimeoutAndThenGivesUp)
{
    openStandard();
    const auto begun = clock.nowMs();

    EXPECT_FALSE(transport.pump(50));
    EXPECT_GE(clock.nowMs() - begun, 50u);
    EXPECT_LT(clock.nowMs() - begun, 60u);
}

TEST_F(UsbTransport, ThePumpReturnsAsSoonAsAReadCompletesWhileItWaits)
{
    openStandard();
    transport.pump(0);
    int waits = 0;

    device.onWait = [&] {
        if (++waits == 3)
        {
            device.deliver(2, event(0x0f, 4));
        }
    };
    const auto begun = clock.nowMs();

    EXPECT_TRUE(transport.pump(1000));
    EXPECT_LT(clock.nowMs() - begun, 10u);
    EXPECT_EQ(event(0x0f, 4), nextEvent());
}

TEST_F(UsbTransport, AnErrorOrAnEmptyReadQueuesNothingAndIsStartedAgain)
{
    openStandard();
    transport.pump(0);

    ASSERT_TRUE(device.deliver(1, event(0x0e, 4), /*status=*/4));
    ASSERT_TRUE(device.deliver(2, event(0x0e, 4), /*status=*/0, /*frames=*/0));
    EXPECT_FALSE(transport.pump(0));
    EXPECT_EQ(UsbBytes(), nextEvent());
    EXPECT_EQ(2, device.startCount(1));
    EXPECT_EQ(2, device.startCount(2));
    EXPECT_NE(std::string::npos, transport.describe("test").find("errors 1"));
    EXPECT_NE(std::string::npos, transport.describe("test").find("empty 1"));
}

TEST_F(UsbTransport, ARefusedStartIsCountedLoggedAndTriedAgainOnTheNextPump)
{
    openStandard();
    device.failStartAt = {5};

    transport.pump(0);
    EXPECT_EQ(0, device.startCount(5));
    EXPECT_TRUE(sink.has("hci: USB_FS_START 5 errno=5"));
    EXPECT_NE(std::string::npos, transport.describe("test").find("start errors 1"));
    device.failStartAt.clear();
    transport.pump(0);
    EXPECT_EQ(1, device.startCount(5));
}

// ---- writing ----

TEST_F(UsbTransport, ACommandGoesOutAsAControlRequestWithItsOpcodeAndLength)
{
    openStandard();
    const std::uint8_t params[] = {0x01, 0x02, 0x03};

    ASSERT_TRUE(transport.command(0x0c03, params, 3));
    ASSERT_EQ(1u, device.controls.size());
    EXPECT_EQ(0, device.controls[0].interface);
    EXPECT_EQ((UsbBytes{0x03, 0x0c, 3, 0x01, 0x02, 0x03}), device.controls[0].data);
    ASSERT_TRUE(transport.command(0x0c03, nullptr, 0));
    EXPECT_EQ((UsbBytes{0x03, 0x0c, 0}), device.controls[1].data);
}

TEST_F(UsbTransport, TheCommandsInterfaceIsTheOneOfTheLayout)
{
    transport.select("ugen1.2", HciLayout{2, 0x81, 0x82, 0x02});
    ASSERT_TRUE(transport.open());
    ASSERT_TRUE(transport.command(0x1005, nullptr, 0));
    EXPECT_EQ(2, device.controls[0].interface);
}

TEST_F(UsbTransport, ARefusedCommandIsReportedAndLogged)
{
    openStandard();
    device.failControl = true;

    EXPECT_FALSE(transport.command(0x1005, nullptr, 0));
    EXPECT_TRUE(sink.has("hci: command 0x1005 failed, errno=5"));
}

TEST_F(UsbTransport, AnAclPacketIsCopiedToTheOutTransferAndStarted)
{
    openStandard();
    const UsbBytes packet = aclPacket(0x0042, 10);

    ASSERT_TRUE(transport.sendAcl(packet.data(), static_cast<std::uint32_t>(packet.size())));
    EXPECT_EQ(packet, device.sentBuffer(kAclOut));
    ASSERT_EQ(1u, device.starts.size());
    EXPECT_EQ(kAclOut, device.starts[0].first);
    EXPECT_EQ(packet.size(), device.starts[0].second);
}

TEST_F(UsbTransport, ASecondAclPacketWaitsUntilTheFirstHasGone)
{
    openStandard();
    const UsbBytes first = aclPacket(0x0042, 10);
    const UsbBytes second = aclPacket(0x0042, 12);

    ASSERT_TRUE(transport.sendAcl(first.data(), static_cast<std::uint32_t>(first.size())));
    int waits = 0;

    device.onWait = [&] {
        if (++waits == 4)
        {
            device.deliver(kAclOut, {});
        }
    };
    EXPECT_TRUE(transport.sendAcl(second.data(), static_cast<std::uint32_t>(second.size())));
    EXPECT_EQ(4, waits);
    EXPECT_EQ(second, device.sentBuffer(kAclOut));
}

TEST_F(UsbTransport, AnAclPacketThatNeverLeavesFailsAfterOneSecond)
{
    openStandard();
    const UsbBytes packet = aclPacket(0x0042, 10);
    const auto begun = clock.nowMs();

    ASSERT_TRUE(transport.sendAcl(packet.data(), static_cast<std::uint32_t>(packet.size())));
    EXPECT_FALSE(transport.sendAcl(packet.data(), static_cast<std::uint32_t>(packet.size())));
    EXPECT_GE(clock.nowMs() - begun, 1000u);
    EXPECT_TRUE(sink.has("hci: ACL out still busy after 1 s"));
}

TEST_F(UsbTransport, APacketLargerThanTheTransferBufferIsRefused)
{
    openStandard();
    const UsbBytes huge(kUsbTransferBytes + 1, 0);

    EXPECT_FALSE(transport.sendAcl(huge.data(), static_cast<std::uint32_t>(huge.size())));
    EXPECT_TRUE(device.starts.empty());
    EXPECT_TRUE(sink.has("exceeds the transfer buffer"));
}

TEST_F(UsbTransport, ARefusedOutStartIsReported)
{
    openStandard();
    device.failStartAt = {kAclOut};
    const UsbBytes packet = aclPacket(0x0042, 4);

    EXPECT_FALSE(transport.sendAcl(packet.data(), static_cast<std::uint32_t>(packet.size())));
    EXPECT_TRUE(sink.has("hci: USB_FS_START 64 errno=5"));
}

// ---- closing ----

TEST_F(UsbTransport, ClosingStopsTheRunningTransfersAndTakesTheTableDown)
{
    openStandard();
    transport.pump(0);
    device.deliver(0, event(0x0e, 4)); // one has finished, so it is no longer running
    transport.pump(0);                 // it is collected here
    transport.pump(0);                 // and started again here

    transport.close();
    EXPECT_EQ(64u, device.stops.size()); // all 48 + 16 reads were running
    EXPECT_TRUE(device.closed);
    EXPECT_TRUE(sink.has("hci (close):"));
}

TEST_F(UsbTransport, ClosingAnUnopenedTransportDoesNothing)
{
    transport.close();
    EXPECT_FALSE(device.closed);
    EXPECT_TRUE(device.stops.empty());
}

TEST_F(UsbTransport, ADestroyedTransportClosesTheDeviceToo)
{
    {
        UsbHciTransport scoped(device, clock, kConsoleNode);

        scoped.select("ugen1.2", HciLayout{});
        ASSERT_TRUE(scoped.open());
        scoped.pump(0);
    }
    EXPECT_TRUE(device.closed);
    EXPECT_EQ(64u, device.stops.size());
}

TEST_F(UsbTransport, OpeningAgainStartsWithEmptyQueuesAndFreshCounters)
{
    openStandard();
    transport.pump(0);
    device.deliver(0, event(0x0e, 4));
    transport.pump(0);
    transport.close();

    ASSERT_TRUE(transport.open());
    EXPECT_EQ(UsbBytes(), nextEvent());
    EXPECT_NE(std::string::npos, transport.describe("test").find("transfers 0 "));
}

// ---- finding the roles of the endpoints ----

namespace
{
// Command Complete for Read Buffer Size, as the controllers answer it
const UsbBytes kReadBufferSizeAnswer = {0x0e, 0x0b, 0x01, 0x05, 0x10, 0x00, 0x36, 0x01, 0x40, 0x0a, 0x00, 0x08, 0x00};
} // namespace

TEST_F(UsbTransport, RoleDetectionSendsReadBufferSizeAndSeesTheAnswerOnTheEventPipe)
{
    openStandard();
    device.onWait = [&] {
        if (!device.controls.empty())
        {
            device.deliver(5, kReadBufferSizeAnswer);
        }
    };

    EXPECT_EQ(1, transport.detectRoles(5000));
    ASSERT_FALSE(device.controls.empty());
    EXPECT_EQ((UsbBytes{0x05, 0x10, 0}), device.controls[0].data);
    EXPECT_TRUE(sink.has("hci: role detection -> 1"));
}

TEST_F(UsbTransport, RoleDetectionSeesAnswerOnTheSecondPipe)
{
    openStandard();
    device.onWait = [&] {
        if (!device.controls.empty())
        {
            device.deliver(kFirstAcl + 1, kReadBufferSizeAnswer);
        }
    };

    EXPECT_EQ(2, transport.detectRoles(5000));
    EXPECT_TRUE(sink.has("hci: role detection -> 2"));
}

TEST_F(UsbTransport, RoleDetectionWithoutAnAnswerGivesUpAndAsksAgainMeanwhile)
{
    openStandard();
    const auto begun = clock.nowMs();

    EXPECT_EQ(0, transport.detectRoles(4000));
    EXPECT_GE(clock.nowMs() - begun, 4000u);
    EXPECT_GE(device.controls.size(), 2u); // the command is repeated every 1.5 s
    EXPECT_TRUE(sink.has("hci: role detection -> 0"));
}

TEST_F(UsbTransport, RoleDetectionIgnoresOtherTraffic)
{
    openStandard();
    device.onWait = [&] {
        if (!device.controls.empty())
        {
            device.deliver(3, event(0x05, 4)); // some other event
        }
    };

    EXPECT_EQ(0, transport.detectRoles(2000));
}
