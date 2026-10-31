// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_fixture.hpp"
#include "core/bluetooth/hci_defs.hpp"

using namespace bluev;
using namespace bluev::testing;
using namespace bluev::hci;

namespace
{

class Link : public BtFixture
{
  protected:
    // the usual start: the adapter is up, the speaker is paired before
    void bondedAndStarted()
    {
        bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
        startExternal();
    }
    Bytes inboundRequest(const BdAddr& who, std::uint32_t cod, std::uint8_t type = 1)
    {
        Bytes p(who.begin(), who.end());
        p.push_back(cod & 0xff);
        p.push_back((cod >> 8) & 0xff);
        p.push_back(cod >> 16);
        p.push_back(type);
        return p;
    }
};

} // namespace

TEST_F(Link, ConnectsToAPairedDeviceAndEncryptsTheLink)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();

    EXPECT_TRUE(stack.linkUp());
    EXPECT_EQ(0x42, stack.handle());
    EXPECT_EQ("Creative", stack.peerName());
    EXPECT_EQ("00:11:22:AA:BB:CC", stack.peerAddress());
    EXPECT_TRUE(sink.has("connecting to 00:11:22:AA:BB:CC Creative (paired before)"));
    EXPECT_TRUE(sink.has("connection complete: handle 0x042"));
    EXPECT_TRUE(sink.has("authentication complete"));
    EXPECT_TRUE(sink.has("link encrypted"));
    EXPECT_TRUE(sink.has("connect: paging "));
    EXPECT_TRUE(sink.has(" s, security "));
    // the stored key went to the controller
    const SentCommand* reply = controller.lastCommand(kLinkKeyReply);
    ASSERT_NE(nullptr, reply);
    EXPECT_EQ(22u, reply->params.size());
    EXPECT_EQ(0x5a, reply->params[6]);
    EXPECT_FALSE(controller.sawCommand(kSetConnEncryption)); // the link encrypted itself
    EXPECT_TRUE(controller.sawCommand(kRoleDiscovery));
}

TEST_F(Link, ForcesEncryptionWhenTheLinkDoesNotDoItAlone)
{
    bondedAndStarted();
    controller.remote.encrypts = false;
    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
    EXPECT_TRUE(controller.sawCommand(kSetConnEncryption));
    EXPECT_EQ(1, controller.lastCommand(kSetConnEncryption)->params[2]);
}

TEST_F(Link, AKnownDeviceThatDoesNotEncryptIsAskedAfterHalfASecond)
{
    bondedAndStarted();
    controller.remote.encrypts = false;
    const auto t0 = clock.nowMs();
    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();

    EXPECT_TRUE(controller.sawCommand(kSetConnEncryption));
    EXPECT_LT(clock.nowMs() - t0, 2000u);
    EXPECT_TRUE(sink.has("connect: paging "));
}

TEST_F(Link, PairsANewDeviceAndSavesTheKey)
{
    startExternal();
    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();

    EXPECT_TRUE(sink.has("(new device)"));
    EXPECT_TRUE(sink.has("no stored key: pairing"));
    EXPECT_TRUE(controller.sawCommand(kLinkKeyNegReply));
    const SentCommand* io = controller.lastCommand(kIoCapabilityReply);
    ASSERT_NE(nullptr, io);
    EXPECT_EQ(3, io->params[6]); // NoInputNoOutput
    EXPECT_TRUE(controller.sawCommand(kUserConfirmReply));
    EXPECT_TRUE(sink.has("paired with 00:11:22:AA:BB:CC, key saved"));

    BondKey saved;
    ASSERT_TRUE(bonds.loadKey(controller.localAddress, remoteAddress(), saved));
    EXPECT_EQ(0xa5, saved.key[0]);
    EXPECT_EQ(4, saved.type);
}

TEST_F(Link, TheNameFromTheInquiryIsStoredWithTheDevice)
{
    InquiryDevice d;
    d.address = remoteAddress();
    d.name = "Creative Pebble";
    controller.inquiryDevices = {d};
    startExternal();
    ASSERT_EQ(1, stack.scan(10000));

    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
    EXPECT_EQ("Creative Pebble", bonds.loadName(remoteAddress()));
    EXPECT_EQ("Creative Pebble", stack.peerName());
    // the page used what the inquiry told about the device
    const SentCommand* page = controller.lastCommand(kCreateConn);
    ASSERT_NE(nullptr, page);
    EXPECT_EQ(controller.inquiryDevices[0].pageScanRepMode, page->params[8]);
    EXPECT_EQ(0x34, page->params[10]);
    EXPECT_EQ(0x92, page->params[11]); // 0x1234 | 0x8000
}

TEST_F(Link, ATelevisionKeyIsNotKept)
{
    InquiryDevice tv;
    tv.address = remoteAddress();
    tv.classOfDevice = 0x24042c;
    tv.name = "Living room";
    controller.inquiryDevices = {tv};
    startExternal();
    stack.scan(10000);

    stack.connectTo(remoteAddress());
    EXPECT_TRUE(sink.has("not saving key for TV/speaker 00:11:22:AA:BB:CC"));
    EXPECT_FALSE(bonds.hasKey(controller.localAddress, remoteAddress()));
}

TEST_F(Link, AKeyThatCannotBeWrittenIsReported)
{
    bonds.failWrites = true;
    startExternal();
    stack.connectTo(remoteAddress());
    EXPECT_TRUE(sink.has("link key: cannot write the key"));
    EXPECT_FALSE(sink.has("key saved"));
}

TEST_F(Link, AStaleKeyIsForgottenAfterAFailedAuthentication)
{
    bondedAndStarted();
    controller.remote.authFails = true;
    EXPECT_FALSE(stack.connectTo(remoteAddress()));
    EXPECT_TRUE(sink.has("authentication failed: Authentication Failure (status 0x06)") ||
                sink.has("authentication failed:"));
    EXPECT_TRUE(sink.has("forgetting the stored key; pair the device again"));
    EXPECT_FALSE(bonds.hasKey(controller.localAddress, remoteAddress()));
}

TEST_F(Link, NoAnswerToAuthenticationTakesThirtySeconds)
{
    bondedAndStarted();
    controller.silentOpcodes = {kAuthRequested};
    const auto t0 = clock.nowMs();
    EXPECT_FALSE(stack.connectTo(remoteAddress()));
    EXPECT_GE(clock.nowMs() - t0, 30000u);
    EXPECT_TRUE(sink.has("authentication: no answer"));
    EXPECT_TRUE(sink.has("security 30.")); // the time the failed step took
    EXPECT_TRUE(sink.has("(failed)"));
}

TEST_F(Link, PagesFourTimesWhenTheDeviceIsSilent)
{
    bondedAndStarted();
    controller.remote.page = FakeController::Remote::Page::Silent;
    EXPECT_FALSE(stack.connectTo(remoteAddress()));

    EXPECT_EQ(4, controller.countCommand(kCreateConn));
    EXPECT_EQ(4, controller.countCommand(kCreateConnCancel));
    EXPECT_TRUE(sink.has("connection: no answer (try 1)"));
    EXPECT_TRUE(sink.has("connection: no answer (try 4)"));
    EXPECT_TRUE(sink.has("connect: no link after "));
    // the first try has DM1 only and no role switch, the second the usual
    std::vector<const SentCommand*> pages;
    for (const SentCommand& c : controller.commands)
    {
        if (c.opcode == kCreateConn)
        {
            pages.push_back(&c);
        }
    }
    EXPECT_EQ(0x08, pages[0]->params[6]);
    EXPECT_EQ(0, pages[0]->params[12]);
    EXPECT_EQ(0x18, pages[1]->params[6]);
    EXPECT_EQ(1, pages[1]->params[12]);
}

TEST_F(Link, ADeviceThatDialsInWhilePagingStillConnects)
{
    bondedAndStarted();
    controller.remote.page = FakeController::Remote::Page::Silent;
    // the device answers during the pause after the first cancel
    controller.event(0x03, {0, 0x42, 0x00, 0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00, 1, 0}, 5000);
    controller.event(0x08, {0, 0x42, 0x00, 1}, 8000);
    controller.event(0x06, {0, 0x42, 0x00}, 7000);
    EXPECT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
    EXPECT_EQ(1, controller.countCommand(kCreateConnCancel));
}

TEST_F(Link, ReportsAFailedPageWithAHint)
{
    bondedAndStarted();
    controller.remote.page = FakeController::Remote::Page::Fail;
    controller.remote.failStatus = 0x04;
    EXPECT_FALSE(stack.connectTo(remoteAddress()));
    EXPECT_TRUE(sink.has("connection failed: "));
    EXPECT_TRUE(sink.has("(the device may be off, out of range or connected to something else)"));
    EXPECT_FALSE(stack.linkUp());
}

TEST_F(Link, AnExistingLinkIsCleanedAndTriedAgain)
{
    bondedAndStarted();
    controller.remote.page = FakeController::Remote::Page::AlreadyConnected;
    EXPECT_FALSE(stack.connectTo(remoteAddress()));

    EXPECT_TRUE(sink.has("ACL already exists, dropping leftover links on this controller"));
    EXPECT_EQ(8, controller.countCommand(kDisconnect));
    EXPECT_EQ(2, controller.countCommand(kCreateConn));
    EXPECT_TRUE(sink.has("an earlier link to this device is still there"));
    ASSERT_FALSE(sink.notes().empty());
    EXPECT_EQ("BlueV: turn the device off and on, then try again", sink.notes().back());
}

TEST_F(Link, TheConsolesChipPausesItsScanOnlyForThePage)
{
    bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
    controller.scanSetting = 3;
    startInternal();
    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();

    std::vector<int> order;
    for (const SentCommand& c : controller.commands)
    {
        if (c.opcode == kWriteScanEnable)
        {
            order.push_back(c.params[0]);
        }
        if (c.opcode == kCreateConn)
        {
            order.push_back(-1);
        }
    }
    EXPECT_EQ((std::vector<int>{0, -1, 3}), order);
}

TEST_F(Link, TheConsolesChipListensWithItsScanOnBetweenPages)
{
    bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
    controller.remote.page = FakeController::Remote::Page::Silent;
    startInternal();
    EXPECT_FALSE(stack.connectTo(remoteAddress()));

    EXPECT_EQ(4, controller.countCommand(kCreateConn));
    EXPECT_EQ(2, controller.lastCommand(kWriteScanEnable)->params[0]);
}

TEST_F(Link, AnAdapterLeavesPageScanAloneWhilePaging)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress())) << sink.all();
    EXPECT_FALSE(controller.sawCommand(kWriteScanEnable));
}

TEST_F(Link, TheConsolesChipDropsNoOtherLinkWhenOneExists)
{
    bonds.put(controller.localAddress, remoteAddress(), 0x5a, "Creative");
    startInternal();
    controller.remote.page = FakeController::Remote::Page::AlreadyConnected;
    EXPECT_FALSE(stack.connectTo(remoteAddress()));
    EXPECT_FALSE(controller.sawCommand(kDisconnect));
}

TEST_F(Link, DisconnectSendsTheRequestAndLeavesPageScanOn)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress()));
    controller.commands.clear();

    stack.disconnect();
    const SentCommand* d = controller.lastCommand(kDisconnect);
    ASSERT_NE(nullptr, d);
    EXPECT_EQ(0x42, d->params[0]);
    EXPECT_EQ(0x13, d->params[2]);
    EXPECT_TRUE(controller.sawCommand(kCreateConnCancel));
    EXPECT_EQ(2, controller.lastCommand(kWriteScanEnable)->params[0]);
    EXPECT_FALSE(stack.linkUp());
    EXPECT_EQ(0, stack.handle());
    EXPECT_TRUE(sink.has("disconnected: "));
}

TEST_F(Link, TheRemoteHangingUpIsNoticed)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress()));
    EXPECT_FALSE(stack.linkLost());

    controller.remoteDisconnect(0x13);
    stack.poll(10);
    EXPECT_TRUE(stack.linkLost());
    EXPECT_FALSE(stack.linkUp());
    EXPECT_TRUE(sink.has("disconnected: "));
    EXPECT_TRUE(sink.has("(reason 0x13)"));
}

TEST_F(Link, AnotherLinkClosingIsOnlyLogged)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress()));
    controller.event(0x05, {0, 0x07, 0x00, 0x08}); // the pad's link
    stack.poll(10);
    EXPECT_FALSE(stack.linkLost());
    EXPECT_TRUE(sink.has("another link closed: handle 0x007"));
}

TEST_F(Link, LinkResetForgetsTheState)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress()));
    stack.linkReset();
    EXPECT_FALSE(stack.linkUp());
    EXPECT_EQ(0, stack.handle());
    EXPECT_FALSE(stack.linkLost());
}

// ---- the device dials in ----
TEST_F(Link, AcceptsAPairedDeviceThatDialsInAndAdoptsTheLink)
{
    bondedAndStarted();
    controller.event(0x04, inboundRequest(remoteAddress(), 0x240404));
    stack.poll(10);

    const SentCommand* accept = controller.lastCommand(kAcceptConnReq);
    ASSERT_NE(nullptr, accept);
    EXPECT_EQ(0, accept->params[6]); // we become the master
    EXPECT_TRUE(sink.has("the device is connecting to us by itself"));

    ASSERT_TRUE(stack.wait([&] { return stack.linkUp(); }, 2000));
    EXPECT_EQ("Creative", stack.peerName()); // the bond of the device was loaded
    ASSERT_TRUE(stack.adoptIncoming()) << sink.all();
    EXPECT_TRUE(sink.has("Creative connected by itself"));
    EXPECT_TRUE(sink.has("link encrypted"));
    EXPECT_TRUE(sink.has("connect: security "));
}

TEST_F(Link, IgnoresADeviceThatIsNotPaired)
{
    startExternal();
    controller.event(0x04, inboundRequest(remoteAddress(), 0x240404));
    controller.event(0x04, inboundRequest(remoteAddress(), 0x240404));
    stack.poll(10);

    EXPECT_FALSE(controller.sawCommand(kAcceptConnReq));
    EXPECT_EQ(2, stack.foreignRequests());
    EXPECT_TRUE(sink.has("inbound connection from 00:11:22:AA:BB:CC (") &&
                sink.has("ignored: not paired with this adapter, #2"));
}

TEST_F(Link, OtherRadioLinkTypesAreLeftAlone)
{
    bondedAndStarted();
    controller.event(0x04, inboundRequest(remoteAddress(), 0x240404, 0)); // SCO
    stack.poll(10);
    EXPECT_FALSE(controller.sawCommand(kAcceptConnReq));
    EXPECT_EQ(0, stack.foreignRequests());
}

TEST_F(Link, AdoptsAPairedDeviceThatConnectsWhileBusy)
{
    bondedAndStarted();
    // a connection complete from a paired device we did not expect
    controller.event(0x03, {0, 0x42, 0x00, 0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00, 1, 0});
    stack.poll(10);
    EXPECT_TRUE(stack.linkUp());
    EXPECT_EQ(0x42, stack.handle());
    EXPECT_EQ("Creative", stack.peerName());
}

TEST_F(Link, ADeviceThatReconnectsWhileTearingDownKeepsItsLink)
{
    bondedAndStarted();
    ASSERT_TRUE(stack.connectTo(remoteAddress()));
    // it hangs up and dials in again right away
    controller.remoteDisconnect(0x13, 20);
    controller.event(0x03, {0, 0x43, 0x00, 0xCC, 0xBB, 0xAA, 0x22, 0x11, 0x00, 1, 0}, 20);
    stack.disconnect();
    EXPECT_TRUE(stack.linkUp()) << sink.all();
    EXPECT_EQ(0x43, stack.handle()) << sink.all();
}
