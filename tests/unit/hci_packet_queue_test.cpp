// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <vector>

#include "capture_sink.hpp"
#include "core/bluetooth/hci_packet_queue.hpp"

using namespace bluev;

namespace
{
using Bytes = std::vector<std::uint8_t>;

// an event: code, parameter length, parameters
Bytes event(std::uint8_t code, std::uint8_t length, std::uint8_t fill = 0xaa)
{
    Bytes packet = {code, length};
    packet.insert(packet.end(), length, fill);
    return packet;
}

// ACL data: handle, then the data length (little endian), then the data
Bytes acl(std::uint16_t handle, std::uint16_t length, std::uint8_t fill = 0xbb)
{
    Bytes packet = {static_cast<std::uint8_t>(handle), static_cast<std::uint8_t>(handle >> 8),
                    static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8)};
    packet.insert(packet.end(), length, fill);
    return packet;
}

Bytes join(const Bytes& first, const Bytes& second)
{
    Bytes both = first;
    both.insert(both.end(), second.begin(), second.end());
    return both;
}

Bytes take(HciPacketQueue& queue)
{
    std::uint8_t out[HciPacketQueue::kSlotBytes];
    const int n = queue.pop(out, sizeof out);

    return Bytes(out, out + n);
}
} // namespace

TEST(HciPacketQueue, APacketComesOutWhole)
{
    HciPacketQueue queue;
    const Bytes e = event(0x0e, 4);

    queue.pushTransfer(e.data(), static_cast<std::uint32_t>(e.size()), HciPacketKind::Event);
    EXPECT_EQ(1, queue.count());
    EXPECT_EQ(e, take(queue));
    EXPECT_EQ(0, queue.count());
    EXPECT_EQ(Bytes(), take(queue)); // empty gives nothing
}

TEST(HciPacketQueue, SeveralEventsInOneReadAreSplit)
{
    HciPacketQueue queue;
    const Bytes both = join(join(event(0x05, 4), event(0x13, 5)), event(0x0f, 4));

    queue.pushTransfer(both.data(), static_cast<std::uint32_t>(both.size()), HciPacketKind::Event);
    ASSERT_EQ(3, queue.count());
    EXPECT_EQ(event(0x05, 4), take(queue));
    EXPECT_EQ(event(0x13, 5), take(queue));
    EXPECT_EQ(event(0x0f, 4), take(queue));
}

TEST(HciPacketQueue, AclDataIsSplitByItsLittleEndianLength)
{
    HciPacketQueue queue;
    const Bytes both = join(acl(0x2042, 300), acl(0x0041, 3));

    queue.pushTransfer(both.data(), static_cast<std::uint32_t>(both.size()), HciPacketKind::Acl);
    ASSERT_EQ(2, queue.count());
    EXPECT_EQ(acl(0x2042, 300), take(queue));
    EXPECT_EQ(acl(0x0041, 3), take(queue));
}

TEST(HciPacketQueue, APacketCutOffAtTheEndIsLeftOut)
{
    HciPacketQueue queue;
    Bytes data = join(event(0x05, 4), event(0x13, 5));

    data.pop_back(); // the second one is one byte short
    queue.pushTransfer(data.data(), static_cast<std::uint32_t>(data.size()), HciPacketKind::Event);
    ASSERT_EQ(1, queue.count());
    EXPECT_EQ(event(0x05, 4), take(queue));
}

TEST(HciPacketQueue, ShortOrEmptyReadsQueueNothing)
{
    HciPacketQueue queue;
    const std::uint8_t one[] = {0x0e};
    const std::uint8_t three[] = {0x01, 0x02, 0x00}; // an ACL header needs four bytes

    queue.pushTransfer(one, 0, HciPacketKind::Event);
    queue.pushTransfer(one, 1, HciPacketKind::Event);
    queue.pushTransfer(three, 3, HciPacketKind::Acl);
    EXPECT_EQ(0, queue.count());
}

TEST(HciPacketQueue, ARealHeaderOnlyPacketCounts)
{
    HciPacketQueue queue;
    const Bytes empty = event(0x08, 0);

    queue.pushTransfer(empty.data(), static_cast<std::uint32_t>(empty.size()), HciPacketKind::Event);
    EXPECT_EQ(1, queue.count());
    EXPECT_EQ(empty, take(queue));
}

TEST(HciPacketQueue, PopCopiesAtMostWhatTheCallerHasRoomFor)
{
    HciPacketQueue queue;
    const Bytes e = event(0x0e, 10);
    std::uint8_t small[4];

    queue.pushTransfer(e.data(), static_cast<std::uint32_t>(e.size()), HciPacketKind::Event);
    EXPECT_EQ(4, queue.pop(small, sizeof small));
    EXPECT_EQ(0x0e, small[0]);
    EXPECT_EQ(0, queue.count()); // the rest of that packet is gone
}

TEST(HciPacketQueue, ADroppedOldestPacketIsLoggedWhenTheQueueIsFull)
{
    bluev::testing::CaptureSink sink;
    HciPacketQueue queue;

    for (int i = 0; i < HciPacketQueue::kSlots; i++)
    {
        const Bytes e = event(static_cast<std::uint8_t>(i), 1);

        queue.pushTransfer(e.data(), static_cast<std::uint32_t>(e.size()), HciPacketKind::Event);
    }
    EXPECT_EQ(HciPacketQueue::kSlots, queue.count());
    EXPECT_FALSE(sink.has("queue full"));

    const Bytes extra = event(0x77, 1);

    queue.pushTransfer(extra.data(), static_cast<std::uint32_t>(extra.size()), HciPacketKind::Event);
    EXPECT_TRUE(sink.has("hci: queue full, dropping oldest packet"));
    EXPECT_EQ(HciPacketQueue::kSlots, queue.count());
    EXPECT_EQ(event(1, 1), take(queue)); // packet 0 was dropped
}

TEST(HciPacketQueue, TheOrderSurvivesTheRingWrappingAround)
{
    HciPacketQueue queue;

    // push and pop more packets than there are slots, a few at a time
    for (int round = 0; round < 40; round++)
    {
        for (int i = 0; i < 3; i++)
        {
            const Bytes e = event(static_cast<std::uint8_t>(round * 3 + i), 1);

            queue.pushTransfer(e.data(), static_cast<std::uint32_t>(e.size()), HciPacketKind::Event);
        }
        for (int i = 0; i < 3; i++)
        {
            ASSERT_EQ(event(static_cast<std::uint8_t>(round * 3 + i), 1), take(queue));
        }
    }
    EXPECT_EQ(0, queue.count());
}

TEST(HciPacketQueue, AClearedQueueIsEmptyAndUsable)
{
    HciPacketQueue queue;
    const Bytes e = event(0x0e, 2);

    queue.pushTransfer(e.data(), static_cast<std::uint32_t>(e.size()), HciPacketKind::Event);
    queue.clear();
    EXPECT_EQ(0, queue.count());
    queue.pushTransfer(e.data(), static_cast<std::uint32_t>(e.size()), HciPacketKind::Event);
    EXPECT_EQ(e, take(queue));
}

TEST(HciPacketQueue, APacketAsBigAsTheReadBufferIsKept)
{
    HciPacketQueue queue;
    const Bytes big = acl(0x0042, HciPacketQueue::kSlotBytes - 4); // fills a USB read exactly

    queue.pushTransfer(big.data(), static_cast<std::uint32_t>(big.size()), HciPacketKind::Acl);
    EXPECT_EQ(big, take(queue));
}

TEST(ReadBufferSizeComplete, OnlyThatCommandCompleteMatches)
{
    // Command Complete, length 11, one command allowed, opcode 0x1005, status 0, then the sizes
    const Bytes answer = {0x0e, 0x0b, 0x01, 0x05, 0x10, 0x00, 0x36, 0x01, 0x40, 0x0a, 0x00, 0x08, 0x00};

    EXPECT_TRUE(isReadBufferSizeComplete(answer.data(), static_cast<std::uint32_t>(answer.size())));
    Bytes other = answer;
    other[3] = 0x09; // the answer to another command
    EXPECT_FALSE(isReadBufferSizeComplete(other.data(), static_cast<std::uint32_t>(other.size())));
    other = answer;
    other[0] = 0x05; // not a Command Complete
    EXPECT_FALSE(isReadBufferSizeComplete(other.data(), static_cast<std::uint32_t>(other.size())));
    other = answer;
    other[1] = 0x0a; // the length byte does not agree with the packet
    EXPECT_FALSE(isReadBufferSizeComplete(other.data(), static_cast<std::uint32_t>(other.size())));
    EXPECT_FALSE(isReadBufferSizeComplete(answer.data(), 5)); // too short to say
}
