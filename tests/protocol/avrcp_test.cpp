// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_fixture.hpp"
#include "core/audio/avrcp.hpp"

using namespace bluev;
using namespace bluev::testing;

namespace
{

class AvrcpChannel : public BtFixture
{
  protected:
    void SetUp() override
    {
        bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Headset");
        startExternal();
        ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
        controller.aclOut.clear();
    }

    // the headset opens the channel (psm 0x17 from its cid 0x0160) and finishes the configuration
    void headsetOpensChannel()
    {
        controller.l2cap(1, {0x02, 0x21, 4, 0, 0x17, 0x00, 0x60, 0x01});
        stack.poll(10);
        controller.l2cap(1, {0x04, 0x22, 8, 0, 0x44, 0x00, 0, 0, 1, 2, 0x90, 0x02});
        controller.l2cap(1, {0x05, 0x01, 6, 0, 0x60, 0x01, 0, 0, 0, 0});
        stack.poll(10);
    }

    // the AVCTP frames the stack sent on the headset's cid 0x0160
    std::vector<Bytes> avctpOut()
    {
        std::vector<Bytes> frames;
        for (const Bytes& pkt : controller.aclOut)
        {
            if (pkt.size() > 8 && (pkt[6] | pkt[7] << 8) == 0x0160)
            {
                frames.emplace_back(pkt.begin() + 8, pkt.end());
            }
        }
        return frames;
    }
};

} // namespace

TEST_F(AvrcpChannel, RegistersForVolumeOnTheChannelTheHeadsetOpened)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel();
    avrcp.start();

    EXPECT_TRUE(sink.has("avrcp: the headset opened the channel"));
    const auto frames = avctpOut();
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(18u, frames[0].size());
    EXPECT_EQ(0x0d, frames[0][13]); // event: volume changed
}

TEST_F(AvrcpChannel, LogsTheVolumeAndRegistersAgainAfterAChange)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel();
    avrcp.start();
    controller.aclOut.clear();

    controller.l2cap(0x44, {0x02, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x40});
    stack.poll(10);
    EXPECT_TRUE(sink.has("avrcp: headset volume 64 of 127"));
    EXPECT_EQ(64, avrcp.volume());
    EXPECT_TRUE(avctpOut().empty()); // an interim answer needs nothing more

    controller.l2cap(0x44, {0x12, 0x11, 0x0e, 0x0d, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x50});
    stack.poll(10);
    EXPECT_TRUE(sink.has("avrcp: headset volume 80 of 127"));
    EXPECT_EQ(80, avrcp.volume());
    EXPECT_EQ(1u, avctpOut().size()); // a notification answers once
}

TEST_F(AvrcpChannel, ACommandFromTheHeadsetIsAnsweredNotImplemented)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel();
    avrcp.start();
    controller.aclOut.clear();

    controller.l2cap(0x44, {0x70, 0x11, 0x0e, 0x01, 0x48, 0x7c, 0x44, 0x00});
    stack.poll(10);
    const auto frames = avctpOut();
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(0x72, frames[0][0]);
    EXPECT_EQ(0x08, frames[0][3]);
}

TEST_F(AvrcpChannel, ARequestedVolumeIsSentOnTheNextServiceAndTakenFromTheAnswer)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel();
    avrcp.start();
    controller.l2cap(0x44, {0x02, 0x11, 0x0e, 0x0f, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x02, 0x0d, 0x40});
    stack.poll(10);
    controller.aclOut.clear();

    avrcp.setVolume(90);
    EXPECT_TRUE(avctpOut().empty()); // nothing goes out before service() runs
    avrcp.service();
    const auto frames = avctpOut();
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(0x50, frames[0][9]); // SET_ABSOLUTE_VOLUME
    EXPECT_EQ(90, frames[0][13]);

    controller.l2cap(0x44, {0x12, 0x11, 0x0e, 0x09, 0x48, 0x00, 0x00, 0x19, 0x58, 0x50, 0x00, 0x00, 0x01, 0x5a});
    stack.poll(10);
    EXPECT_EQ(90, avrcp.volume());
}

TEST_F(AvrcpChannel, NoVolumeIsSentBeforeTheHeadsetHasToldItsOwn)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel();
    avrcp.start();
    controller.aclOut.clear();

    avrcp.setVolume(90);
    avrcp.service();
    EXPECT_TRUE(avctpOut().empty());
}

TEST_F(AvrcpChannel, ARejectedRegistrationMeansNoVolumeOnThisLinkOnly)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel();
    avrcp.start();
    controller.l2cap(0x44, {0x02, 0x11, 0x0e, 0x0a, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31, 0x00, 0x00, 0x01, 0x00});
    stack.poll(10);
    EXPECT_TRUE(sink.has("no volume control on this link"));
    EXPECT_EQ(-1, avrcp.volume());

    controller.aclOut.clear();
    avrcp.service(); // it is not asked again on this link
    EXPECT_TRUE(avctpOut().empty());

    // the next link asks again
    avrcp.reset();
    headsetOpensChannel();
    avrcp.start();
    EXPECT_EQ(1u, avctpOut().size());
}

TEST_F(AvrcpChannel, StartDoesNotWaitForTheHeadset)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    const auto t0 = clock.nowMs();
    avrcp.start();
    EXPECT_LT(clock.nowMs() - t0, 100u); // the stream is not held up
    EXPECT_TRUE(sink.has("avrcp: waiting for the headset to open the channel"));

    // six ticks for the headset to open it, then one attempt of ours
    for (int i = 0; i < 6; i++)
    {
        avrcp.service();
    }
    EXPECT_FALSE(sink.has("avrcp: channel opened by us"));
    avrcp.service();
    EXPECT_TRUE(sink.has("avrcp: channel not open yet, waiting for the headset"));
}

TEST_F(AvrcpChannel, ATargetSearchStopsUsFromRacingTheHeadset)
{
    // the headset opens an SDP channel and searches for our target record
    controller.l2cap(1, {0x02, 0x01, 4, 0, 0x00, 0x01, 0x60, 0x01});
    stack.poll(10);
    // Service Search, pattern {0x110c}, max 2 records
    controller.l2cap(0x50, {0x02, 0x00, 0x01, 0x00, 0x08, 0x35, 0x03, 0x19, 0x11, 0x0c, 0x00, 0x02, 0x00});
    stack.poll(10);

    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    controller.aclOut.clear();
    avrcp.start();
    EXPECT_TRUE(sink.has("avrcp: the headset is opening the channel"));
    // it never asks for psm 0x17 itself; that request is what the headset
    // refuses while it is opening its own
    bool asked = false;
    for (const Bytes& pkt : controller.aclOut)
    {
        if (pkt.size() > 8 && (pkt[6] | pkt[7] << 8) == 1)
        {
            const Bytes payload(pkt.begin() + 8, pkt.end());
            if (payload.size() >= 8 && payload[0] == 0x02 && (payload[6] | payload[7] << 8) == 0x17)
            {
                asked = true;
            }
        }
    }
    EXPECT_FALSE(asked);
}

TEST_F(AvrcpChannel, AHeadsetThatOpensTheChannelLateIsRegisteredByService)
{
    Avrcp avrcp(stack);

    avrcp.reset();
    avrcp.listen();
    headsetOpensChannel(); // start() has not run, or has given up waiting
    controller.aclOut.clear();
    avrcp.service();
    EXPECT_EQ(1u, avctpOut().size());
}
