// SPDX-License-Identifier: GPL-3.0-or-later
// Puts the fragments of ACL packets back together into L2CAP frames.
#pragma once

#include <cstdint>

#include "core/bytes.hpp"

namespace bluev
{

class L2capReassembler
{
  public:
    // One ACL packet's payload; `flags` is the handle field of its header.
    // True when a whole frame is ready: read it with cid(), payload() and
    // payloadLength(), then release() it. A continuation with no start, or one
    // that does not fit, is dropped.
    bool feed(std::uint16_t flags, const std::uint8_t* payload, std::uint32_t length);

    std::uint16_t cid() const
    {
        return getLe16(m_buf + 2);
    }
    const std::uint8_t* payload() const
    {
        return m_buf + 4;
    }
    int payloadLength() const
    {
        return static_cast<int>(m_need) - 4;
    }

    // done with the frame (it stays valid until then)
    void release()
    {
        m_need = 0;
    }
    // drops a frame in progress (a new link)
    void reset()
    {
        m_need = 0;
        m_len = 0;
    }

  private:
    std::uint8_t m_buf[4096];
    std::uint32_t m_need = 0; // bytes of the frame including its header; 0 = none in progress
    std::uint32_t m_len = 0;  // bytes so far
};

} // namespace bluev
