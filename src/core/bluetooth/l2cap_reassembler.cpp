// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bluetooth/l2cap_reassembler.hpp"

#include <cstring>

#include "core/bytes.hpp"

namespace bluev
{

bool L2capReassembler::feed(std::uint16_t flags, const std::uint8_t* pl, std::uint32_t dlen)
{
    if ((flags & 0x3000) == 0x1000)
    { // continuation fragment
        if (m_need == 0)
        {
            return false;
        }
        if (m_len + dlen > 0x1000)
        {
            m_need = 0;
            return false;
        }
        std::memcpy(m_buf + m_len, pl, dlen);
        m_len += dlen;
        return m_len >= m_need;
    }
    if (dlen >= 4 && dlen <= 0x1000)
    { // start fragment
        const std::uint16_t l2len = getLe16(pl);

        std::memcpy(m_buf, pl, dlen);
        m_len = dlen;
        m_need = static_cast<std::uint32_t>(l2len) + 4;
        return m_need <= m_len;
    }
    return false;
}

} // namespace bluev
