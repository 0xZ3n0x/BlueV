// SPDX-License-Identifier: GPL-3.0-or-later
// inquiry and the list of devices
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/device_filter.hpp"
#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{
// Inquiry: GIAC LAP 0x9E8B33, ~10.24 s, unlimited responses
const std::uint8_t kInquiryParams[5] = {0x33, 0x8b, 0x9e, 0x18, 0x00};
constexpr std::size_t kMaxDevices = 16;
} // namespace

void BtStack::onInquiryRecord(const std::uint8_t* rec, std::int32_t rssi, const std::uint8_t* eir, int eirLength)
{
    const std::uint8_t* cod = rec + 8;
    const BdAddr address = addressFrom(rec);

    // class of device: only care about Audio/Video major class
    if ((cod[1] & 0x1f) != 4)
    {
        if (m_skippedLogged < 40)
        {
            m_skippedLogged++;
            const unsigned cls = static_cast<unsigned>(cod[0] | cod[1] << 8 | cod[2] << 16);

            logLine("inquiry: skipped %s (%s, class %06x), not audio/video", addressText(address).c_str(),
                    deviceClassName(cls), cls);
        }
        return;
    }

    const std::string name = eirName(eir, eirLength);

    BtDevice* dev = nullptr;
    for (BtDevice& d : m_devices)
    {
        if (d.address == address)
        {
            dev = &d;
            break;
        }
    }
    const bool isNew = dev == nullptr;
    if (isNew)
    {
        if (m_devices.size() >= kMaxDevices)
        {
            return;
        }
        m_devices.emplace_back();
        dev = &m_devices.back();
        dev->address = address;
        dev->rssi = -128;
    }

    dev->pageScanRepMode = rec[6];
    dev->clockOffset = static_cast<std::uint16_t>((rec[11] | rec[12] << 8) | 0x8000);
    dev->classOfDevice = static_cast<std::uint32_t>(cod[0]) | cod[1] << 8 | cod[2] << 16;
    if (rssi > -128)
    {
        dev->rssi = rssi;
    }

    bool nameChanged = false;
    if (!name.empty())
    {
        nameChanged = dev->name.empty() || dev->name != name;
        dev->name = name;
    }

    if (nameChanged || isNew)
    {
        const char* tag = isJunkDevice(dev->classOfDevice, dev->name) ? " [ignored]" : "";

        logLine("found %s '%s' (%s, class %06x, rssi %d)%s", addressText(dev->address).c_str(), dev->name.c_str(),
                deviceClassName(dev->classOfDevice), dev->classOfDevice, dev->rssi, tag);
    }
}

// Starts an inquiry and retries until `until` when the controller is
// already inquiring (0x0c), which happens when the console's own stack is
// scanning. Returns the last status, or -1 when the status reply went to the
// console's read.
int BtStack::startInquiry(std::uint64_t until)
{
    int status = -1;

    for (int attempt = 1;; attempt++)
    {
        status = commandStatus(hci::kInquiry, kInquiryParams, 5, 1000);
        if (status <= 0)
        {
            return status;
        }
        logLine("inquiry: the controller refused to start (%s, status %#04x), attempt %d", hciStatusName(status),
                status, attempt);
        if (m_clock.nowMs() >= until)
        {
            return status;
        }
        cmd(hci::kInquiryCancel);
        poll(500);
    }
}

void BtStack::runInquiry(std::uint32_t timeoutMs)
{
    m_inqDone = false;
    m_inquiryStopped = false;
    // The console's stack can leave an Inquiry Result event filter behind;
    // clear it, or the shared controller reports no inquiry result at all.
    const std::uint8_t all = 0x00;
    cmd(hci::kSetEventFilter, &all, 1);
    poll(50);

    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;
    std::uint64_t firstUntil = m_clock.nowMs() + 1600;
    if (firstUntil > deadline)
    {
        firstUntil = deadline;
    }
    m_inquiryActive = true;
    int status = startInquiry(firstUntil);
    if (status > 0)
    {
        logLine("inquiry: could not start (%s)", hciStatusName(status));
        m_inquiryActive = false;
        return;
    }
    if (status < 0)
    {
        // the status reply can go to the console's own read on the shared chip
        logLine("inquiry: no status from the controller, assuming it started");
    }

    while (!m_inqDone)
    {
        if (m_inquiryStopped)
        {
            // The console's Inquiry Cancel stops the inquiry on the shared
            // controller; start ours again with the time that is left.
            m_inquiryStopped = false;
            m_inqDone = false;
            if (m_clock.nowMs() >= deadline)
            {
                break;
            }
            logLine("inquiry: the console stopped it, starting it again");
            poll(500);
            status = startInquiry(deadline);
            if (status > 0)
            {
                logLine("inquiry: could not start it again (%s)", hciStatusName(status));
                break;
            }
            continue;
        }
        if (m_clock.nowMs() >= deadline)
        {
            cmd(hci::kInquiryCancel);
            poll(200);
            break;
        }
        poll(100);
        if (m_connDone && m_connStatus == 0)
        {
            cmd(hci::kInquiryCancel);
            poll(200);
            break;
        }
    }
    m_inquiryActive = false;
}

int BtStack::scan(std::uint32_t timeoutMs)
{
    m_devices.clear();
    logLine("scan: %u s", timeoutMs / 1000);
    runInquiry(timeoutMs);
    logLine("scan: %d device(s)", static_cast<int>(m_devices.size()));
    return static_cast<int>(m_devices.size());
}

} // namespace bluev
