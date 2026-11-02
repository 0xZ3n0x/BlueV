// SPDX-License-Identifier: GPL-3.0-or-later
// The HCI packets that a controller sent, waiting for the stack to take them.
// A USB read may hold several packets back to back; they are split here and
// queued one by one. The queue has a fixed size and drops its oldest packet
// when it is full, so a stack that is busy loses old traffic, not new.
#pragma once

#include <cstdint>

namespace bluev
{

// Where a packet begins: an event has a two byte header (code, length), ACL
// data a four byte one (handle, length).
enum class HciPacketKind
{
    Event,
    Acl
};

class HciPacketQueue
{
  public:
    static constexpr int kSlots = 32;
    static constexpr int kSlotBytes = 0x44c;

    // Queues every whole packet in `data` (a USB read of `length` bytes); a
    // packet cut off at the end is ignored.
    void pushTransfer(const std::uint8_t* data, std::uint32_t length, HciPacketKind kind);
    // The oldest packet, at most `max` bytes of it; its length, or 0 when empty.
    int pop(std::uint8_t* out, int max);
    // Packets waiting.
    int count() const
    {
        return m_count;
    }
    void clear()
    {
        m_head = 0;
        m_count = 0;
    }

  private:
    void push(const std::uint8_t* data, std::uint32_t length);

    std::uint8_t m_buf[kSlots][kSlotBytes];
    std::uint32_t m_len[kSlots];
    int m_head = 0;
    int m_count = 0;
};

// Whether `packet` (an event of `length` bytes) is the Command Complete for
// Read Buffer Size (opcode 0x1005): the answer that role detection waits for.
bool isReadBufferSizeComplete(const std::uint8_t* packet, std::uint32_t length);

} // namespace bluev
