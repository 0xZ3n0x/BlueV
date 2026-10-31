// SPDX-License-Identifier: GPL-3.0-or-later
// inquiry and the list of devices
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/device_filter.hpp"
#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{
// Inquiry: GIAC LAP 0x9E8B33, ~10.24 s, unlimited responses
const std::uint8_t kInquiryParams[5] = {0x33, 0x8b, 0x9e, 0x18, 0x00};
constexpr std::size_t kMaxFound = 16;
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
            const unsigned cls = getLe24(cod);

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
        if (m_devices.size() >= kMaxFound)
        {
            return;
        }
        m_devices.emplace_back();
        dev = &m_devices.back();
        dev->address = address;
        dev->rssi = -128;
    }

    dev->pageScanRepMode = rec[6];
    dev->clockOffset = static_cast<std::uint16_t>(getLe16(rec + 11) | 0x8000);
    dev->classOfDevice = getLe24(cod);
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

void BtStack::runInquiry(std::uint32_t timeoutMs)
{
    m_inqDone = false;
    if (!cmd(hci::kInquiry, kInquiryParams, 5))
    {
        return;
    }
    // until the inquiry ends, or the device we dialled for has connected
    wait([this] { return m_inqDone || (m_connDone && m_connStatus == 0); }, timeoutMs, false);
    if (!m_inqDone)
    {
        cmd(hci::kInquiryCancel);
        poll(200);
    }
}

int BtStack::scan(std::uint32_t timeoutMs)
{
    m_devices.clear();
    logLine("scan: %u s", timeoutMs / 1000);
    // the console's own inquiry scan takes the time an inquiry needs; page scan
    // stays, a gamepad can always reach the console
    const bool pauseInquiryScan = !m_external && (m_systemScan & 1) != 0;
    if (pauseInquiryScan)
    {
        setScan(m_systemScan & 2);
    }
    runInquiry(timeoutMs);
    if (pauseInquiryScan)
    {
        restoreScan();
    }
    logLine("scan: %d device(s)", static_cast<int>(m_devices.size()));
    if (m_devices.empty() && m_external && m_family != FirmwareFamily::Unknown && m_family != FirmwareFamily::Csr &&
        !firmwareUploadSupported(m_family))
    {
        logLine("scan: nothing found; if this %s adapter needs firmware, BlueV cannot upload it yet",
                firmwareFamilyName(m_family));
    }
    return static_cast<int>(m_devices.size());
}

} // namespace bluev
