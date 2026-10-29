// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <vector>

#include "core/bluetooth/usb_layout.hpp"

using namespace bluev;

namespace
{

using Bytes = std::vector<std::uint8_t>;

void add(Bytes& to, const Bytes& what)
{
    to.insert(to.end(), what.begin(), what.end());
}

// configuration header: total length filled in at the end
Bytes configuration(const Bytes& body, int interfaces)
{
    Bytes bytes = {9, 2, 0, 0, static_cast<std::uint8_t>(interfaces), 1, 0, 0xc0, 0};

    add(bytes, body);
    bytes[2] = static_cast<std::uint8_t>(bytes.size() & 0xff);
    bytes[3] = static_cast<std::uint8_t>(bytes.size() >> 8);
    return bytes;
}

Bytes interfaceDescriptor(int number, int alternate, int endpoints, int cls = 0xe0, int sub = 1, int protocol = 1)
{
    return {9,
            4,
            static_cast<std::uint8_t>(number),
            static_cast<std::uint8_t>(alternate),
            static_cast<std::uint8_t>(endpoints),
            static_cast<std::uint8_t>(cls),
            static_cast<std::uint8_t>(sub),
            static_cast<std::uint8_t>(protocol),
            0};
}

Bytes endpoint(int address, int attributes, int maxPacket)
{
    return {7,
            5,
            static_cast<std::uint8_t>(address),
            static_cast<std::uint8_t>(attributes),
            static_cast<std::uint8_t>(maxPacket & 0xff),
            static_cast<std::uint8_t>(maxPacket >> 8),
            1};
}

// the three endpoints of a controller: an interrupt in, then two bulk
Bytes controller(int number, int events, int bulkA, int bulkB)
{
    Bytes bytes = interfaceDescriptor(number, 0, 3);

    add(bytes, endpoint(events, 3, 16));
    add(bytes, endpoint(bulkA, 2, 64));
    add(bytes, endpoint(bulkB, 2, 64));
    return bytes;
}

} // namespace

TEST(UsbLayout, TheSlimOrderOfAnAdapter)
{
    // what a CSR adapter reports: interrupt 0x81, bulk out 0x02, bulk in 0x82, then the audio interface
    Bytes body = controller(0, 0x81, 0x02, 0x82);
    add(body, interfaceDescriptor(1, 0, 2));
    add(body, endpoint(0x03, 1, 0));
    add(body, endpoint(0x83, 1, 0));
    const Bytes config = configuration(body, 2);

    HciLayout layout;
    ASSERT_TRUE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
    EXPECT_EQ(0, layout.interface);
    EXPECT_EQ(0x81, layout.events);
    EXPECT_EQ(0x82, layout.aclIn);
    EXPECT_EQ(0x02, layout.aclOut);
}

TEST(UsbLayout, TheFatConsoleChipHasTheNumbersTheOtherWayRound)
{
    const Bytes config = configuration(controller(0, 0x82, 0x01, 0x81), 1);

    HciLayout layout;
    ASSERT_TRUE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
    EXPECT_EQ(0x82, layout.events);
    EXPECT_EQ(0x81, layout.aclIn);
    EXPECT_EQ(0x01, layout.aclOut);
}

TEST(UsbLayout, TheBulkOutWithTheNumberOfTheBulkInWins)
{
    // two bulk outs: 0x05 first, then 0x02 which has the number of the bulk in 0x82
    Bytes body = interfaceDescriptor(0, 0, 4);
    add(body, endpoint(0x81, 3, 16));
    add(body, endpoint(0x05, 2, 64));
    add(body, endpoint(0x82, 2, 64));
    add(body, endpoint(0x02, 2, 64));
    const Bytes config = configuration(body, 1);

    HciLayout layout;
    ASSERT_TRUE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
    EXPECT_EQ(0x82, layout.aclIn);
    EXPECT_EQ(0x02, layout.aclOut);
}

TEST(UsbLayout, WithoutAMatchingNumberTheFirstBulkOutIsUsed)
{
    Bytes body = interfaceDescriptor(0, 0, 4);
    add(body, endpoint(0x81, 3, 16));
    add(body, endpoint(0x05, 2, 64));
    add(body, endpoint(0x82, 2, 64));
    add(body, endpoint(0x07, 2, 64));
    const Bytes config = configuration(body, 1);

    HciLayout layout;
    ASSERT_TRUE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
    EXPECT_EQ(0x05, layout.aclOut);
}

TEST(UsbLayout, TheFirstBluetoothInterfaceCountsAndOtherInterfacesAreSkipped)
{
    // a vendor interface first, then the Bluetooth interface 3, then a second group on interface 5
    Bytes body = controller(1, 0x8a, 0x0a, 0x8b);
    body[5] = 0xff; // not class e0
    add(body, controller(3, 0x8c, 0x0b, 0x8d));
    add(body, controller(5, 0x85, 0x04, 0x84));
    const Bytes config = configuration(body, 3);

    HciLayout layout;
    ASSERT_TRUE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
    EXPECT_EQ(3, layout.interface);
    EXPECT_EQ(0x8c, layout.events);
    EXPECT_EQ(0x8d, layout.aclIn);
    EXPECT_EQ(0x0b, layout.aclOut);
}

TEST(UsbLayout, AnAlternateSettingIsNotTheDefaultOne)
{
    Bytes body = interfaceDescriptor(0, 1, 3);
    add(body, endpoint(0x81, 3, 16));
    add(body, endpoint(0x82, 2, 64));
    add(body, endpoint(0x02, 2, 64));
    const Bytes config = configuration(body, 1);

    HciLayout layout;
    EXPECT_FALSE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
}

TEST(UsbLayout, AnInterfaceMissingAnEndpointIsNotUsable)
{
    Bytes body = interfaceDescriptor(0, 0, 2);
    add(body, endpoint(0x81, 3, 16));
    add(body, endpoint(0x82, 2, 64));
    const Bytes config = configuration(body, 1);

    HciLayout layout;
    layout.events = 0x77;
    EXPECT_FALSE(findHciLayout(config.data(), static_cast<int>(config.size()), layout));
    EXPECT_EQ(0x77, layout.events) << "nothing is written when nothing is found";
}

TEST(UsbLayout, ACutShortOrBrokenDescriptorIsHandled)
{
    const Bytes config = configuration(controller(0, 0x81, 0x02, 0x82), 1);
    HciLayout layout;

    // cut in the middle of the last endpoint (the bulk in): it is dropped, so the interface has no bulk in
    EXPECT_FALSE(findHciLayout(config.data(), static_cast<int>(config.size()) - 4, layout));
    EXPECT_FALSE(findHciLayout(config.data(), 0, layout));
    EXPECT_FALSE(findHciLayout(config.data(), 1, layout));

    Bytes zeroLength = config;
    zeroLength[9] = 0; // the interface descriptor says it is 0 bytes long
    EXPECT_FALSE(findHciLayout(zeroLength.data(), static_cast<int>(zeroLength.size()), layout));
}
