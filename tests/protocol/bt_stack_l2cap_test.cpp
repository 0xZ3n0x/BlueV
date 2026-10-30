// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_fixture.hpp"
#include "core/bluetooth/hci_defs.hpp"

using namespace bluev;
using namespace bluev::testing;
using namespace bluev::hci;

namespace
{

// one signaling command out of an ACL packet the stack sent, or code 0
struct Sig
{
    std::uint8_t code = 0, id = 0;
    Bytes data;
};

bool signalingOf(const Bytes& pkt, Sig& out)
{
    if (pkt.size() < 12 || (pkt[6] | pkt[7] << 8) != 1)
    {
        return false;
    }
    out.code = pkt[8];
    out.id = pkt[9];
    const int len = pkt[10] | pkt[11] << 8;
    out.data.assign(pkt.begin() + 12, pkt.begin() + 12 + len);
    return true;
}

class L2cap : public BtFixture
{
  protected:
    void connected()
    {
        bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
        startExternal();
        ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
        controller.aclOut.clear();
    }

    // the remote answers connection and configuration requests the way a
    // speaker does; `theirCid` is the cid it puts on the channel
    void speaker(std::uint16_t theirCid, std::uint16_t connectResult = 0)
    {
        controller.onAclOut = [this, theirCid, connectResult](const Bytes& pkt) {
            Sig s;
            if (!signalingOf(pkt, s))
            {
                return;
            }
            if (s.code == 0x02)
            { // connection request: psm, scid
                const std::uint16_t scid = s.data[2] | s.data[3] << 8;
                controller.l2cap(1, {0x03, s.id, 8, 0, static_cast<std::uint8_t>(theirCid & 0xff),
                                     static_cast<std::uint8_t>(theirCid >> 8), static_cast<std::uint8_t>(scid & 0xff),
                                     static_cast<std::uint8_t>(scid >> 8), static_cast<std::uint8_t>(connectResult), 0,
                                     0, 0});
                if (connectResult == 0)
                {
                    // its own configure request: MTU 335
                    controller.l2cap(1, {0x04, 0x31, 8, 0, static_cast<std::uint8_t>(scid & 0xff),
                                         static_cast<std::uint8_t>(scid >> 8), 0, 0, 1, 2, 0x4f, 0x01});
                }
            }
            else if (s.code == 0x04)
            { // our configure request
                const std::uint16_t dcid = s.data[0] | s.data[1] << 8;
                controller.l2cap(1, {0x05, s.id, 6, 0, static_cast<std::uint8_t>(dcid & 0xff),
                                     static_cast<std::uint8_t>(dcid >> 8), 0, 0, 0, 0});
            }
            else if (s.code == 0x06)
            { // disconnection request
                controller.l2cap(1, {0x07, s.id, 4, 0, s.data[0], s.data[1], s.data[2], s.data[3]});
            }
        };
    }

    std::vector<Sig> sentSignaling()
    {
        std::vector<Sig> out;
        for (const Bytes& p : controller.aclOut)
        {
            Sig s;
            if (signalingOf(p, s))
            {
                out.push_back(s);
            }
        }
        return out;
    }
    int countSent(std::uint8_t code)
    {
        int n = 0;
        for (const Sig& s : sentSignaling())
        {
            n += s.code == code;
        }
        return n;
    }
};

} // namespace

TEST_F(L2cap, OpensAChannelWeInitiate)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    ASSERT_TRUE(stack.openChannel(ch, 0x19, nullptr)) << sink.all();

    EXPECT_EQ(0x0140u, ch.theirCid);
    EXPECT_TRUE(ch.up);
    EXPECT_TRUE(ch.configured);
    EXPECT_EQ(335u, ch.theirMtu);
    const auto sent = sentSignaling();
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(0x02, sent[0].code); // connection request to psm 0x19
    EXPECT_EQ(0x19, sent[0].data[0]);
    EXPECT_EQ(0x40, sent[0].data[2]);
    EXPECT_EQ(1, countSent(0x04)); // configure request with MTU 672
    EXPECT_EQ(1, countSent(0x05)); // the answer to the speaker's request
}

TEST_F(L2cap, ARefusedChannelIsReported)
{
    connected();
    speaker(0, 3);
    L2Channel ch("media", 0x42);
    EXPECT_FALSE(stack.openChannel(ch, 0x19, nullptr));
    EXPECT_TRUE(sink.has("l2cap: media connection refused (3)"));
}

TEST_F(L2cap, ABusyPeerIsAskedAgain)
{
    connected();
    int attempts = 0;
    controller.onAclOut = [&](const Bytes& pkt) {
        Sig s;
        if (!signalingOf(pkt, s) || s.code != 0x02)
        {
            return;
        }
        const std::uint16_t scid = s.data[2] | s.data[3] << 8;
        const std::uint8_t result = ++attempts <= 2 ? 4 : 0;
        controller.l2cap(1, {0x03, s.id, 8, 0, 0x40, 0x01, static_cast<std::uint8_t>(scid & 0xff),
                             static_cast<std::uint8_t>(scid >> 8), result, 0, 0, 0});
        if (result == 0)
        {
            controller.l2cap(1, {0x04, 0x31, 8, 0, static_cast<std::uint8_t>(scid & 0xff),
                                 static_cast<std::uint8_t>(scid >> 8), 0, 0, 1, 2, 0xa0, 0x02});
        }
    };
    L2Channel ch("signaling", 0x40);
    // the configure request of ours is answered too
    const auto first = controller.onAclOut;
    controller.onAclOut = [&, first](const Bytes& pkt) {
        first(pkt);
        Sig s;
        if (signalingOf(pkt, s) && s.code == 0x04)
        {
            controller.l2cap(1, {0x05, s.id, 6, 0, 0x40, 0x00, 0, 0, 0, 0});
        }
    };
    ASSERT_TRUE(stack.openChannel(ch, 0x19, nullptr)) << sink.all();
    EXPECT_EQ(3, attempts);
    EXPECT_EQ(2, sink.count("peer busy, asking again"));
}

TEST_F(L2cap, NoAnswerTimesOutAfterTenSeconds)
{
    connected();
    L2Channel ch("signaling", 0x40);
    const auto t0 = clock.nowMs();
    EXPECT_FALSE(stack.openChannel(ch, 0x19, nullptr));
    EXPECT_GE(clock.nowMs() - t0, 10000u);
    EXPECT_TRUE(sink.has("l2cap: signaling connection timed out"));
}

TEST_F(L2cap, ASilentPeerAfterConnectContinuesAfterAGracePeriod)
{
    connected();
    // the connection is answered and our configure request too, but the peer never sends its own
    controller.onAclOut = [&](const Bytes& pkt) {
        Sig s;
        if (!signalingOf(pkt, s))
        {
            return;
        }
        if (s.code == 0x02)
        {
            controller.l2cap(1, {0x03, s.id, 8, 0, 0x40, 0x01, s.data[2], s.data[3], 0, 0, 0, 0});
        }
        if (s.code == 0x04)
        {
            controller.l2cap(1, {0x05, s.id, 6, 0, 0x40, 0x00, 0, 0, 0, 0});
        }
    };
    L2Channel ch("signaling", 0x40);
    ASSERT_TRUE(stack.openChannel(ch, 0x19, nullptr));
    EXPECT_TRUE(sink.has("no configure request from the peer, continuing anyway"));
    EXPECT_FALSE(ch.configured);
}

TEST_F(L2cap, FramesOnAnOpenChannelReachItsHandler)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    std::vector<Bytes> got;
    ASSERT_TRUE(stack.openChannel(ch, 0x19, [&](std::uint32_t cid, const std::uint8_t* d, int n) {
        EXPECT_EQ(0x40u, cid);
        got.emplace_back(d, d + n);
    }));

    controller.l2cap(0x40, {0x10, 0x01, 0x02});
    controller.l2cap(0x77, {0x99}); // nobody listens on that
    stack.poll(10);
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ((Bytes{0x10, 0x01, 0x02}), got[0]);
}

TEST_F(L2cap, FragmentedFramesAreReassembled)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    std::vector<Bytes> got;
    ASSERT_TRUE(
        stack.openChannel(ch, 0x19, [&](std::uint32_t, const std::uint8_t* d, int n) { got.emplace_back(d, d + n); }));

    // an L2CAP frame of 6 payload bytes in two ACL packets
    controller.acl({0x42, 0x20, 0x07, 0x00, 0x06, 0x00, 0x40, 0x00, 1, 2, 3}); // start: 7 of 10 bytes
    controller.acl({0x42, 0x10, 0x03, 0x00, 4, 5, 6});                         // continuation
    stack.poll(10);
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ((Bytes{1, 2, 3, 4, 5, 6}), got[0]);
}

TEST_F(L2cap, AContinuationWithoutAStartIsDropped)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    int calls = 0;
    ASSERT_TRUE(stack.openChannel(ch, 0x19, [&](std::uint32_t, const std::uint8_t*, int) { calls++; }));
    controller.acl({0x42, 0x10, 0x03, 0x00, 4, 5, 6});
    stack.poll(10);
    EXPECT_EQ(0, calls);
}

TEST_F(L2cap, PacketsOfOtherLinksAreCountedAndIgnored)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    int calls = 0;
    ASSERT_TRUE(stack.openChannel(ch, 0x19, [&](std::uint32_t, const std::uint8_t*, int) { calls++; }));
    controller.l2cap(0x40, {1, 2, 3}, 0x0007); // the pad's handle
    stack.poll(10);
    EXPECT_EQ(0, calls);
    EXPECT_EQ(1, stack.otherTraffic());
}

TEST_F(L2cap, AcceptsAnInboundChannelOnTheBoundPsm)
{
    connected();
    L2Channel sig("signaling", 0x40);
    std::vector<Bytes> got;
    stack.bindInbound(0x19, sig, [&](std::uint32_t, const std::uint8_t* d, int n) { got.emplace_back(d, d + n); });

    // the speaker connects: psm 0x19 from its cid 0x0150
    controller.l2cap(1, {0x02, 0x21, 4, 0, 0x19, 0x00, 0x50, 0x01});
    stack.poll(10);

    EXPECT_EQ(0x0150u, sig.theirCid);
    EXPECT_TRUE(sig.connDone);
    EXPECT_TRUE(sink.has("l2cap: accepted inbound psm 0x19"));
    const auto sent = sentSignaling();
    ASSERT_EQ(2u, sent.size());
    EXPECT_EQ(0x03, sent[0].code); // connection response: our cid 0x40, result 0
    EXPECT_EQ(0x40, sent[0].data[0]);
    EXPECT_EQ(0x50, sent[0].data[2]);
    EXPECT_EQ(0, sent[0].data[4]);
    EXPECT_EQ(0x04, sent[1].code); // and our configure request

    // the speaker's configure request and the answer to ours
    controller.l2cap(1, {0x04, 0x22, 8, 0, 0x40, 0x00, 0, 0, 1, 2, 0x90, 0x02});
    controller.l2cap(1, {0x05, sent[1].id, 6, 0, 0x50, 0x01, 0, 0, 0, 0});
    stack.poll(10);
    EXPECT_TRUE(sig.configured);
    EXPECT_TRUE(sig.up);
    EXPECT_EQ(656u, sig.theirMtu);

    controller.l2cap(0x40, {7, 7});
    stack.poll(10);
    ASSERT_EQ(1u, got.size());
}

TEST_F(L2cap, UnknownPsmsAreRefusedAndOnlyLoggedTwicePerLink)
{
    connected();
    for (int i = 0; i < 5; i++)
    {
        controller.l2cap(1, {0x02, static_cast<std::uint8_t>(0x30 + i), 4, 0, 0x17, 0x00, 0x60, 0x01});
    }
    stack.poll(10);

    int refused = 0;
    for (const Sig& s : sentSignaling())
    {
        if (s.code == 0x03 && s.data[4] == 2)
        {
            refused++;
        }
    }
    EXPECT_EQ(5, refused);
    EXPECT_EQ(2, sink.count("l2cap: rejected psm 0x17"));
}

TEST_F(L2cap, SdpIsAlwaysServedOnCid0x50)
{
    connected();
    controller.l2cap(1, {0x02, 0x40, 4, 0, 0x01, 0x00, 0x70, 0x01}); // psm 1 from cid 0x0170
    stack.poll(10);
    controller.aclOut.clear();

    // a service search for the audio source
    controller.l2cap(0x50, {0x02, 0x00, 0x09, 0x00, 0x08, 0x35, 0x03, 0x19, 0x11, 0x0a, 0x00, 0x02, 0x00});
    stack.poll(10);
    ASSERT_EQ(1u, controller.aclOut.size());
    const Bytes& r = controller.aclOut[0];
    EXPECT_EQ(0x70, r[6]); // to its cid
    EXPECT_EQ(0x01, r[7]);
    EXPECT_EQ(0x03, r[8]); // service search response
    EXPECT_EQ(1, r[14]);   // one record
}

TEST_F(L2cap, ClosesAChannelAndWaitsForTheAnswer)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    ASSERT_TRUE(stack.openChannel(ch, 0x19, nullptr));
    controller.aclOut.clear();

    stack.closeChannel(ch);
    EXPECT_TRUE(ch.closed);
    EXPECT_EQ(1, countSent(0x06));
    // closing a closed channel does nothing
    stack.closeChannel(ch);
    EXPECT_EQ(1, countSent(0x06));
}

TEST_F(L2cap, TheRemoteClosingAChannelIsNoticed)
{
    connected();
    speaker(0x0140);
    L2Channel ch("signaling", 0x40);
    ASSERT_TRUE(stack.openChannel(ch, 0x19, nullptr));
    controller.aclOut.clear();

    controller.l2cap(1, {0x06, 0x55, 4, 0, 0x40, 0x00, 0x40, 0x01}); // its disconnection request
    stack.poll(10);
    EXPECT_TRUE(ch.closed);
    EXPECT_EQ(1, countSent(0x07));
}

TEST_F(L2cap, EchoAndInformationRequestsAreAnswered)
{
    connected();
    controller.l2cap(1, {0x08, 0x61, 3, 0, 'a', 'b', 'c'});
    controller.l2cap(1, {0x0a, 0x62, 2, 0, 0x03, 0x00}); // extended features
    controller.l2cap(1, {0x0a, 0x63, 2, 0, 0x02, 0x00}); // feature mask
    controller.l2cap(1, {0x0a, 0x64, 2, 0, 0x09, 0x00}); // something else
    stack.poll(10);

    const auto sent = sentSignaling();
    ASSERT_EQ(4u, sent.size());
    EXPECT_EQ(0x09, sent[0].code);
    EXPECT_EQ((Bytes{'a', 'b', 'c'}), sent[0].data);
    EXPECT_EQ(0x0b, sent[1].code);
    EXPECT_EQ(2, sent[1].data[4]); // connectionless data
    EXPECT_EQ(0x0b, sent[2].code);
    EXPECT_EQ(0, sent[2].data[2]);
    EXPECT_EQ(1, sent[3].data[2]); // not supported
}

TEST_F(L2cap, SeveralCommandsInOneFrameAreAllHandled)
{
    connected();
    controller.l2cap(1, {0x08, 0x61, 1, 0, 'x', 0x08, 0x62, 1, 0, 'y'});
    stack.poll(10);
    EXPECT_EQ(2, countSent(0x09));
}

TEST_F(L2cap, ATruncatedCommandStopsTheParse)
{
    connected();
    controller.l2cap(1, {0x08, 0x61, 9, 0, 'x'});
    stack.poll(10);
    EXPECT_EQ(0, countSent(0x09));
}

TEST_F(L2cap, AnEchoTooBigToAnswerIsIgnored)
{
    connected();
    Bytes echo = {0x08, 0x61, 0x90, 0x01}; // 400 bytes of data
    echo.insert(echo.end(), 400, 'e');
    controller.l2cap(1, echo);
    controller.l2cap(1, {0x08, 0x62, 1, 0, 'x'}); // the next command is still served
    stack.poll(10);
    EXPECT_EQ(1, countSent(0x09));
}

TEST_F(L2cap, CommandsShorterThanTheirFixedPartAreSkipped)
{
    connected();
    controller.l2cap(1, {0x03, 0x01, 0, 0});             // connection response with no data
    controller.l2cap(1, {0x04, 0x02, 1, 0, 0x40});       // configure request cut short
    controller.l2cap(1, {0x05, 0x03, 2, 0, 0x40, 0x00}); // configure response cut short
    controller.l2cap(1, {0x06, 0x04, 0, 0});             // disconnection request with no data
    controller.l2cap(1, {0x0a, 0x05, 0, 0});             // information request with no type
    controller.l2cap(1, {0x08, 0x06, 1, 0, 'x'});        // an echo behind them is answered
    stack.poll(10);

    EXPECT_EQ(1, countSent(0x09));
    EXPECT_EQ(0, countSent(0x07)) << "no disconnection response for a request without channels";
    EXPECT_EQ(0, countSent(0x0b));
}

// ---- sending ----
TEST_F(L2cap, SendBuildsTheAclPacketAndSpendsACredit)
{
    connected();
    const int before = stack.credits();
    const Bytes data = {1, 2, 3, 4, 5};
    ASSERT_TRUE(stack.send(0x0140, data.data(), 5));

    ASSERT_EQ(1u, controller.aclOut.size());
    const Bytes& p = controller.aclOut[0];
    EXPECT_EQ(0x42, p[0]);
    EXPECT_EQ(0x20, p[1]); // start, no flush
    EXPECT_EQ(9, p[2]);
    EXPECT_EQ(5, p[4]);
    EXPECT_EQ(0x40, p[6]);
    EXPECT_EQ(0x01, p[7]);
    EXPECT_EQ(3, p[10]);
    EXPECT_EQ(before - 1, stack.credits());
}

TEST_F(L2cap, TooBigFramesAreRefused)
{
    connected();
    Bytes big(0x445, 0);
    EXPECT_FALSE(stack.send(0x0140, big.data(), 0x445));
    EXPECT_TRUE(sink.has("1093-byte frame too large to send unfragmented"));
    EXPECT_TRUE(controller.aclOut.empty());
}

TEST_F(L2cap, TheCreditsFollowTheCompletedPacketsReports)
{
    connected();
    const Bytes data(100, 0);
    for (int i = 0; i < 4; i++)
    {
        ASSERT_TRUE(stack.send(0x0140, data.data(), 100));
    }
    EXPECT_EQ(4, stack.credits());
    EXPECT_EQ(4, stack.reportsMissing());

    controller.completedPackets(0x42, 3);
    controller.completedPackets(0x07, 2); // the pad's packets
    stack.poll(10);
    EXPECT_EQ(7, stack.credits());
    EXPECT_EQ(1, stack.reportsMissing());
    EXPECT_EQ(2, stack.otherTraffic());

    controller.completedPackets(0x42, 20); // never more than the controller has
    stack.poll(10);
    EXPECT_EQ(8, stack.credits());
}

TEST_F(L2cap, SendingIsHeldBackWhenTooMuchIsInFlight)
{
    connected();
    const Bytes data(100, 0);
    for (int i = 0; i < 5; i++)
    {
        EXPECT_TRUE(stack.canSend()) << i;
        ASSERT_TRUE(stack.send(0x0140, data.data(), 100));
    }
    EXPECT_TRUE(stack.canSend()); // five unconfirmed is still fine
    ASSERT_TRUE(stack.send(0x0140, data.data(), 100));
    EXPECT_FALSE(stack.canSend()); // the sixth is too many

    clock.advance(40); // no report after 30 ms: assumed complete
    EXPECT_TRUE(stack.canSend());
    EXPECT_GE(stack.completionsAssumed(), 1);
}

TEST_F(L2cap, NoCreditsNoSending)
{
    connected();
    const Bytes data(10, 0);
    for (int i = 0; i < 8; i++)
    {
        stack.send(0x0140, data.data(), 10);
    }
    EXPECT_EQ(0, stack.credits());
    EXPECT_FALSE(stack.canSend());
}
