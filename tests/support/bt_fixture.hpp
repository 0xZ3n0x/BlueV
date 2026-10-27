// SPDX-License-Identifier: GPL-3.0-or-later
// A BtStack on a fake controller, with a clock, a bond store and a log capture.
#pragma once

#include <gtest/gtest.h>

#include "capture_sink.hpp"
#include "core/bluetooth/bt_stack.hpp"
#include "fake_clock.hpp"
#include "fake_controller.hpp"
#include "fake_stores.hpp"

namespace bluev::testing
{

class BtFixture : public ::testing::Test
{
  protected:
    BtFixture() : controller(clock), stack(controller, bonds, usb, firmware, clock)
    {
        usb.layouts["ugen0.2"] = HciLayout{0, 0x81, 0x82, 0x02};
        usb.layouts["ugen1.2"] = HciLayout{0, 0x81, 0x82, 0x02};
    }

    // bring the stack up on the adapter in the USB port
    void startExternal()
    {
        stack.useAdapter("ugen1.2");
        ASSERT_TRUE(stack.start()) << sink.all();
        controller.commands.clear();
        controller.aclOut.clear();
    }
    void startInternal()
    {
        stack.useAdapter("");
        ASSERT_TRUE(stack.start()) << sink.all();
        controller.commands.clear();
    }

    BdAddr remoteAddress() const
    {
        return controller.remote.address;
    }

    FakeClock clock;
    CaptureSink sink;
    FakeBondStore bonds;
    FakeUsbDirectory usb;
    FakeFirmwareStore firmware;
    FakeController controller;
    BtStack stack;
};

} // namespace bluev::testing
