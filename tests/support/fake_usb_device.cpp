// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_usb_device.hpp"

#include <algorithm>
#include <cstring>

namespace bluev::testing
{

bool FakeUsbDevice::open(const char* nodeName, int transfers, int forceShort)
{
    if (failOpen)
    {
        m_error = errorNumber;
        return false;
    }
    opened = true;
    closed = false;
    node = nodeName;
    transferCount = transfers;
    forceShortIndex = forceShort;
    return true;
}

void FakeUsbDevice::close()
{
    closed = true;
    opened = false;
}

bool FakeUsbDevice::openTransfer(int index, std::uint8_t endpoint, std::uint32_t length)
{
    if (failOpenTransferAt.count(index) != 0)
    {
        m_error = errorNumber;
        return false;
    }
    openedTransfers.push_back({index, endpoint, length});
    return true;
}

bool FakeUsbDevice::start(int index, std::uint32_t length)
{
    if (failStartAt.count(index) != 0)
    {
        m_error = errorNumber;
        return false;
    }
    starts.emplace_back(index, length);
    m_started.insert(index);
    m_startedBuffers.emplace_back(index, UsbBytes(m_buffers[index], m_buffers[index] + length));
    return true;
}

bool FakeUsbDevice::stop(int index)
{
    stops.push_back(index);
    m_started.erase(index);
    return true;
}

bool FakeUsbDevice::deliver(int index, const UsbBytes& data, int status, std::uint32_t frames)
{
    if (m_started.count(index) == 0)
    {
        return false;
    }
    std::memcpy(m_buffers[index], data.data(), std::min<std::size_t>(data.size(), kUsbTransferBytes));
    m_results[index].status = status;
    m_results[index].frames = frames;
    m_results[index].length = static_cast<std::uint32_t>(data.size());
    m_started.erase(index);
    m_completed.push_back(index);
    return true;
}

int FakeUsbDevice::complete()
{
    if (m_completed.empty())
    {
        return -1;
    }
    const int index = m_completed.front();

    m_completed.pop_front();
    return index;
}

bool FakeUsbDevice::control(std::uint8_t interface, const std::uint8_t* data, std::uint16_t length)
{
    if (failControl)
    {
        m_error = errorNumber;
        return false;
    }
    controls.push_back({interface, UsbBytes(data, data + length)});
    return true;
}

void FakeUsbDevice::wait(std::uint32_t microseconds)
{
    waitedMicroseconds += microseconds;
    m_clockRemainder += microseconds;
    m_clock.advance(m_clockRemainder / 1000);
    m_clockRemainder %= 1000;
    if (onWait)
    {
        onWait();
    }
}

int FakeUsbDevice::startCount(int index) const
{
    return static_cast<int>(
        std::count_if(starts.begin(), starts.end(), [index](const auto& s) { return s.first == index; }));
}

UsbBytes FakeUsbDevice::sentBuffer(int index) const
{
    for (auto it = m_startedBuffers.rbegin(); it != m_startedBuffers.rend(); ++it)
    {
        if (it->first == index)
        {
            return it->second;
        }
    }
    return {};
}

} // namespace bluev::testing
