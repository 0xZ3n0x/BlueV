// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/radio.hpp"

namespace bluev
{

Radio::Radio(IHciTransport& transport, IBondStore& bonds, IUsbDirectory& usb, IFirmwareStore& firmware,
             IAudioSource& audio, IClock& clock)
    : m_bt(transport, bonds, usb, firmware, clock), m_session(m_bt, clock),
      m_streamer(m_bt, m_session, audio, clock, {})
{
}

bool Radio::open(const std::string& node)
{
    m_bt.useAdapter(node);
    if (!m_bt.start())
    {
        return false;
    }
    m_session.reset();
    m_session.listen();
    return true;
}

std::string Radio::openError()
{
    return m_bt.startError();
}

void Radio::close()
{
    m_bt.setTick(nullptr);
    m_bt.stop();
}

} // namespace bluev
