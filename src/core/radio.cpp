// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/radio.hpp"

#include "core/bluetooth/device_filter.hpp"

namespace bluev
{

Radio::Radio(IHciTransport& transport, IBondStore& bonds, IUsbDirectory& usb, IAudioSource& audio, IClock& clock)
    : m_bt(transport, bonds, usb, clock), m_session(m_bt, clock), m_streamer(m_bt, m_session, audio, clock, nullptr)
{
}

bool Radio::open(const std::string& node)
{
    m_bt.useAdapter(node);
    if (!m_bt.start())
    {
        return false;
    }
    m_bt.setServiceMode(true);
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

std::vector<RadioDevice> Radio::devices()
{
    std::vector<RadioDevice> out;

    for (const BtDevice& d : m_bt.devices())
    {
        RadioDevice r;
        r.address = d.address;
        r.name = d.name;
        r.rssi = d.rssi;
        r.classOfDevice = d.classOfDevice;
        r.other = isJunkDevice(d.classOfDevice, d.name);
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace bluev
