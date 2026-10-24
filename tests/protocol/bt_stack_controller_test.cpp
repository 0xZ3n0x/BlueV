// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_fixture.hpp"
#include "core/bluetooth/hci_defs.hpp"

using namespace bluev;
using namespace bluev::testing;
using namespace bluev::hci;

TEST_F(BtFixture, StartsAnAdapterWithAReset)
{
    stack.useAdapter("ugen1.2");
    ASSERT_TRUE(stack.start());

    EXPECT_EQ("ugen1.2", controller.selectedNode);
    EXPECT_TRUE(controller.sawCommand(kReset)); // an adapter of ours alone is reset
    EXPECT_TRUE(controller.sawCommand(kWriteSimplePairing));
    EXPECT_TRUE(controller.sawCommand(kWriteLocalName));
    const SentCommand* name = controller.lastCommand(kWriteLocalName);
    ASSERT_NE(nullptr, name);
    EXPECT_EQ(248u, name->params.size());
    EXPECT_EQ("BlueV", std::string(reinterpret_cast<const char*>(name->params.data())));
    const SentCommand* scan = controller.lastCommand(kWriteScanEnable);
    ASSERT_NE(nullptr, scan);
    EXPECT_EQ(2, scan->params[0]); // page scan on at the end
    EXPECT_TRUE(sink.has("controller: 8 ACL buffers of 1021 bytes"));
    EXPECT_TRUE(sink.has("manufacturer 0x000a (CSR)"));
    EXPECT_TRUE(sink.has("controller address 66:55:44:33:22:11"));
    EXPECT_TRUE(sink.has("radio: leftover links cleared"));
}

TEST_F(BtFixture, TheConsolesOwnChipIsNeverReset)
{
    stack.useAdapter("");
    ASSERT_TRUE(stack.start());

    EXPECT_EQ("", controller.selectedNode);
    EXPECT_FALSE(controller.sawCommand(kReset));
    EXPECT_TRUE(sink.has("console chip ugen0.2: Bluetooth interface 0"));
}

TEST_F(BtFixture, DropsLeftoverLinksOnStartAndStop)
{
    startExternal();
    stack.stop();

    int disconnects = controller.countCommand(kDisconnect);
    EXPECT_EQ(8, disconnects); // handles 1..8
    EXPECT_EQ(1, controller.closes);
    // page scan stays on for the console's own pads
    EXPECT_EQ(2, controller.lastCommand(kWriteScanEnable)->params[0]);
}

TEST_F(BtFixture, OpenFailureGivesAReason)
{
    controller.openOk = false;
    stack.useAdapter("ugen1.2");
    EXPECT_FALSE(stack.start());
    EXPECT_EQ("the adapter could not be opened", stack.startError());

    stack.useAdapter("");
    EXPECT_FALSE(stack.start());
    EXPECT_EQ("the console's Bluetooth chip could not be opened", stack.startError());
}

TEST_F(BtFixture, AnAdapterThatDoesNotAnswerTheResetIsRefused)
{
    controller.silentOpcodes = {kReset};
    stack.useAdapter("ugen1.2");
    EXPECT_FALSE(stack.start());
    EXPECT_EQ("the adapter did not answer (HCI Reset)", stack.startError());
    EXPECT_EQ(1, controller.closes);
    EXPECT_TRUE(sink.has("no reply to"));
    EXPECT_EQ(5, controller.countCommand(kReset)); // five tries
}

TEST_F(BtFixture, AnAdapterWithoutClassicIsRefusedButTheConsolesChipIsNot)
{
    controller.bredr = false;
    stack.useAdapter("ugen1.2");
    EXPECT_FALSE(stack.start());
    EXPECT_EQ("this adapter has no Bluetooth Classic (BR/EDR); audio needs it", stack.startError());

    stack.useAdapter("");
    EXPECT_TRUE(stack.start());
}

TEST_F(BtFixture, MissedBufferSizeFallsBackToTheDefaults)
{
    controller.silentOpcodes = {kReadBufferSize};
    stack.useAdapter("ugen1.2");
    ASSERT_TRUE(stack.start());
    EXPECT_EQ(7, stack.credits());
    EXPECT_EQ(1021 - 4 + 0, stack.maxFrame() + 0);
    EXPECT_TRUE(sink.has("buffer size reply missed, using 7 x 1021"));
}

TEST_F(BtFixture, LayoutComesFromTheDescriptors)
{
    usb.layouts["ugen1.2"] = HciLayout{1, 0x83, 0x84, 0x05};
    stack.useAdapter("ugen1.2");
    EXPECT_EQ(1, controller.selectedLayout.interface);
    EXPECT_EQ(0x83, controller.selectedLayout.events);
    EXPECT_EQ(0x05, controller.selectedLayout.aclOut);
    EXPECT_TRUE(sink.has(
        "adapter ugen1.2: Bluetooth interface 1, events 0x83, ACL in 0x84, ACL out 0x5 (from its descriptors)"));
}

TEST_F(BtFixture, AnAdapterWithoutDescriptorsGetsTheStandardLayout)
{
    usb.layouts.clear();
    stack.useAdapter("ugen1.2");
    EXPECT_EQ(0x81, controller.selectedLayout.events);
    EXPECT_EQ(0x82, controller.selectedLayout.aclIn);
    EXPECT_EQ(0x02, controller.selectedLayout.aclOut);
    EXPECT_TRUE(sink.has("descriptors not available, assuming the standard layout"));
}

TEST_F(BtFixture, TheConsolesChipWithoutDescriptorsIsDetectedByBehaviour)
{
    usb.layouts.clear();
    controller.detectResult = 2;
    stack.useAdapter("");
    EXPECT_EQ(0x82, controller.selectedLayout.events); // the guess for the fat console
    ASSERT_TRUE(stack.start());

    EXPECT_EQ(0x81, controller.selectedLayout.events); // switched to the standard layout
    EXPECT_EQ(2, controller.opens);
    EXPECT_TRUE(sink.has("switching to the standard endpoint layout"));

    // remembered for the next time
    stack.useAdapter("");
    EXPECT_EQ(0x81, controller.selectedLayout.events);
}

TEST_F(BtFixture, CommandsToTheConsolesChipAnswerFromTheConsoleAreLoggedOnce)
{
    startExternal();
    // an answer to a command we never sent
    controller.event(0x0e, {1, 0x03, 0x0c, 0x00});
    stack.poll(10);
    EXPECT_TRUE(sink.has("the console's own command answered: "));
    EXPECT_EQ(1, sink.count("the console's own command answered"));

    // answers to our own commands are not
    stack.setScan(0);
    EXPECT_EQ(1, sink.count("the console's own command answered"));
}

TEST_F(BtFixture, ConsoleCommandLogsStopAfterFifty)
{
    startExternal();
    for (int i = 0; i < 80; i++)
    {
        controller.event(0x0e, {1, 0x03, 0x0c, 0x00});
    }
    stack.poll(10);
    EXPECT_EQ(50, sink.count("the console's own command answered"));
}

TEST_F(BtFixture, TheTickRunsAboutOncePerSecond)
{
    startExternal();
    int ticks = 0;
    stack.setTick([&] { ticks++; });
    for (int i = 0; i < 100; i++)
    {
        stack.poll(100); // ten seconds
    }
    EXPECT_GE(ticks, 9);
    EXPECT_LE(ticks, 11);
}

TEST_F(BtFixture, WaitStopsOnTimeoutAndOnLinkLoss)
{
    startExternal();
    const auto before = clock.nowMs();
    EXPECT_FALSE(stack.wait([] { return false; }, 1000));
    EXPECT_GE(clock.nowMs() - before, 1000u);

    int calls = 0;
    EXPECT_TRUE(stack.wait([&] { return ++calls > 3; }, 5000));
}
