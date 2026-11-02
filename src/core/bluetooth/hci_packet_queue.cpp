// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/hci_packet_queue.hpp"

#include <algorithm>
#include <cstring>

#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

void HciPacketQueue::pushTransfer(const std::uint8_t* data, std::uint32_t length, HciPacketKind kind)
{
    const std::uint32_t header = kind == HciPacketKind::Event ? 2 : 4;
    std::uint32_t offset = 0;

    while (offset + header <= length)
    {
        const std::uint32_t payload = kind == HciPacketKind::Event ? data[offset + 1] : getLe16(data + offset + 2);
        const std::uint32_t total = payload + header;

        if (offset + total > length)
        {
            break;
        }
        push(data + offset, total);
        offset += total;
    }
}

void HciPacketQueue::push(const std::uint8_t* data, std::uint32_t length)
{
    if (m_count == kSlots)
    {
        logLine("hci: queue full, dropping oldest packet");
        m_head = (m_head + 1) % kSlots;
        m_count--;
    }
    const int slot = (m_head + m_count) % kSlots;
    const std::uint32_t kept = std::min<std::uint32_t>(length, kSlotBytes); // no packet is larger than a USB read

    std::memcpy(m_buf[slot], data, kept);
    m_len[slot] = kept;
    m_count++;
}

int HciPacketQueue::pop(std::uint8_t* out, int max)
{
    if (m_count == 0)
    {
        return 0;
    }
    const int n = std::min<int>(static_cast<int>(m_len[m_head]), max);

    std::memcpy(out, m_buf[m_head], static_cast<std::size_t>(n));
    m_head = (m_head + 1) % kSlots;
    m_count--;
    return n;
}

bool isReadBufferSizeComplete(const std::uint8_t* packet, std::uint32_t length)
{
    // Command Complete (0x0e), its length, the number of commands, the opcode 0x1005 (little endian)
    return length >= 6 && packet[0] == 0x0e && packet[1] == length - 2 && packet[3] == 0x05 && packet[4] == 0x10;
}

} // namespace bluev
