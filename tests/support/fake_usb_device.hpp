// SPDX-License-Identifier: GPL-3.0-or-later
// A USB device as the kernel would give it, played by a test: the transfer
// table, what was started and stopped, and transfers that a test finishes.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/ports/usb_device.hpp"
#include "fake_clock.hpp"

namespace bluev::testing
{

using UsbBytes = std::vector<std::uint8_t>;

class FakeUsbDevice final : public IUsbDevice
{
  public:
    explicit FakeUsbDevice(FakeClock& clock) : m_clock(clock)
    {
    }

    // ---- how it behaves ----
    bool failOpen = false;            // the node cannot be opened
    std::set<int> failOpenTransferAt; // these indexes always fail to open
    std::set<int> failStartAt;        // starting these transfers fails
    bool failControl = false;
    int errorNumber = 5; // what lastError() says after a failure
    // called each time the transport waits, so that a test can finish transfers while time passes
    std::function<void()> onWait;

    // ---- what a test does to it ----
    // Finishes a started transfer with `data` in its buffer; false when it was not started.
    bool deliver(int index, const UsbBytes& data, int status = 0, std::uint32_t frames = 1);

    // ---- what it saw ----
    bool opened = false;
    bool closed = false;
    std::string node;
    int transferCount = 0;
    int forceShortIndex = -1;
    struct Opened
    {
        int index;
        std::uint8_t endpoint;
        std::uint32_t length;
    };
    std::vector<Opened> openedTransfers;
    std::vector<std::pair<int, std::uint32_t>> starts; // every start: index, length
    std::vector<int> stops;
    struct Control
    {
        std::uint8_t interface;
        UsbBytes data;
    };
    std::vector<Control> controls;
    std::uint64_t waitedMicroseconds = 0;
    bool started(int index) const
    {
        return m_started.count(index) != 0;
    }
    int startCount(int index) const;
    UsbBytes sentBuffer(int index) const; // the buffer of a started transfer, as handed over

    // ---- IUsbDevice ----
    bool open(const char* nodeName, int transfers, int forceShort) override;
    void close() override;
    bool openTransfer(int index, std::uint8_t endpoint, std::uint32_t length) override;
    std::uint8_t* buffer(int index) override
    {
        return m_buffers[index];
    }
    bool start(int index, std::uint32_t length) override;
    bool stop(int index) override;
    int complete() override;
    UsbTransferResult result(int index) override
    {
        return m_results[index];
    }
    bool control(std::uint8_t interface, const std::uint8_t* data, std::uint16_t length) override;
    void wait(std::uint32_t microseconds) override;
    int lastError() const override
    {
        return m_error;
    }

  private:
    FakeClock& m_clock;
    std::set<int> m_started;
    std::deque<int> m_completed;
    std::uint8_t m_buffers[kUsbTransfers][kUsbTransferBytes] = {};
    UsbTransferResult m_results[kUsbTransfers];
    std::vector<std::pair<int, UsbBytes>> m_startedBuffers;
    int m_error = 0;
    std::uint64_t m_clockRemainder = 0; // microseconds not yet added to the clock
};

} // namespace bluev::testing
