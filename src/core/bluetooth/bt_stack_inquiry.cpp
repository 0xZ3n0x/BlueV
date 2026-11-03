// SPDX-License-Identifier: GPL-3.0-or-later
// inquiry and the list of devices
#include <algorithm>
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
// The console's stack shares the controller and holds it while it inquires or
// pages; the Inquiry then comes back "command not allowed now".
constexpr int kInquiryTries = 6;
constexpr std::uint32_t kInquiryStatusMs = 2500;
// A device that does not name itself in its inquiry answer: how many get a name
// request after a scan, and how long each may take
constexpr std::size_t kMaxNames = 4;
constexpr std::uint32_t kNameStatusMs = 1500;
constexpr std::uint32_t kNameMs = 2000;
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

// A device that answers the inquiry without its name (Sony headsets do) is
// named with a Remote Name Request, strongest first, at most a few. Without it
// the list shows only the address and the device cannot be told apart.
void BtStack::fetchNames()
{
    std::vector<BtDevice*> todo;

    for (BtDevice& d : m_devices)
    {
        if (d.name.empty())
        {
            todo.push_back(&d);
        }
    }
    std::sort(todo.begin(), todo.end(), [](const BtDevice* a, const BtDevice* b) { return a->rssi > b->rssi; });
    for (std::size_t i = 0; i < todo.size() && i < kMaxNames; i++)
    {
        fetchName(*todo[i]);
    }
}

void BtStack::fetchName(BtDevice& device)
{
    std::uint8_t request[10];

    std::memcpy(request, device.address.data(), 6);
    request[6] = device.pageScanRepMode;
    request[7] = 0;
    putLe16(request + 8, device.clockOffset);
    m_nameStatusDone = false;
    m_nameStatus = -1;
    m_nameDone = false;
    m_namePeer = device.address;
    m_name.clear();
    if (!cmd(hci::kRemoteNameRequest, request, 10))
    {
        return;
    }
    wait([this] { return m_nameStatusDone; }, kNameStatusMs, false);
    if (!m_nameStatusDone || m_nameStatus != 0)
    {
        logLine("scan: name of %s not requested (%s, status %#04x)", addressText(device.address).c_str(),
                m_nameStatusDone ? hciStatusName(static_cast<unsigned>(m_nameStatus)) : "no answer", m_nameStatus);
        return;
    }
    wait([this] { return m_nameDone; }, kNameMs, false);
    if (m_nameDone && m_nameStatus == 0 && !m_name.empty())
    {
        device.name = m_name;
        logLine("scan: %s is '%s'", addressText(device.address).c_str(), device.name.c_str());
        return;
    }
    // a request still running holds the controller for the next one
    cmd(hci::kRemoteNameRequestCancel, device.address.data(), 6);
    poll(100);
}

void BtStack::runInquiry(std::uint32_t timeoutMs)
{
    m_inqDone = false;
    m_inqStatusDone = false;
    m_inqStatus = -1;
    m_inqResults = 0;
    const std::uint64_t deadline = m_clock.nowMs() + timeoutMs;

    // On the console's chip the console's stack writes its own event mask while
    // it works, and that turns the inquiry results off. Put ours back first.
    if (!m_external)
    {
        setEventMask();
    }

    bool started = false;
    for (int attempt = 1; attempt <= kInquiryTries && !started; attempt++)
    {
        if (!cmd(hci::kInquiry, kInquiryParams, 5))
        {
            logLine("scan: the inquiry was not sent");
            return;
        }
        m_inqStatusDone = false;
        wait([this] { return m_inqStatusDone; }, kInquiryStatusMs, false);
        if (m_inqStatusDone && m_inqStatus == 0)
        {
            started = true;
            break;
        }
        if (!m_inqStatusDone)
        {
            logLine("scan: no answer to the inquiry (try %d)", attempt);
            continue;
        }
        logLine("scan: inquiry refused (%s, status %#04x, try %d)", hciStatusName(static_cast<unsigned>(m_inqStatus)),
                m_inqStatus, attempt);
        // the console's stack is inquiring or paging; wait for it to be done
        if (m_inqStatus != 0x0c && m_inqStatus != 0x0d && m_inqStatus != 0x3a)
        {
            break;
        }
        poll(1000);
    }
    if (!started)
    {
        logLine("scan: the inquiry did not start");
        return;
    }

    // until the inquiry ends, or the device we dialled for has connected.
    // The console's stack can write its mask again during the scan, so a scan
    // that hears no result re-arms it a few times before it gives up.
    const std::uint64_t end = m_clock.nowMs() + (deadline > m_clock.nowMs() ? deadline - m_clock.nowMs() : 1000);
    std::uint64_t lastResult = m_clock.nowMs();
    int resultsSeen = m_inqResults;
    unsigned rearmed = 0;
    while (!m_inqDone && !(m_connDone && m_connStatus == 0) && m_clock.nowMs() < end)
    {
        poll(200);
        if (m_inqResults != resultsSeen)
        {
            resultsSeen = m_inqResults;
            lastResult = m_clock.nowMs();
        }
        // a scan that has heard nothing for 3 s: the mask is ours again
        if (!m_external && rearmed < 3 && m_clock.nowMs() - lastResult >= 3000)
        {
            rearmed++;
            lastResult = m_clock.nowMs();
            setEventMask(2);
        }
    }
    if (!m_inqDone)
    {
        cmd(hci::kInquiryCancel);
        poll(200);
    }
    if (rearmed > 0)
    {
        logLine("scan: the event mask was written %u more time(s) during the scan", rearmed);
    }
}

int BtStack::scan(std::uint32_t timeoutMs)
{
    m_devices.clear();
    logLine("scan: %u s", timeoutMs / 1000);
    // On the console's chip the console's page scan runs at nearly full duty and
    // leaves an inquiry little radio time, so both scans are off for the scan.
    // A gamepad cannot reach the console for those seconds.
    const bool pauseScan = !m_external && m_systemScan != 0;
    if (pauseScan)
    {
        setScan(0);
    }
    runInquiry(timeoutMs);
    if (pauseScan)
    {
        restoreScan();
    }
    if (m_inqResults > 0)
    {
        fetchNames();
    }
    logLine("scan: %d device(s)", static_cast<int>(m_devices.size()));
    if (m_devices.empty() && m_inqResults == 0)
    {
        logLine("scan: no inquiry result came back; the controller's event mask may have been changed");
    }
    if (m_devices.empty() && m_external && m_family != FirmwareFamily::Unknown && m_family != FirmwareFamily::Csr &&
        !firmwareUploadSupported(m_family))
    {
        logLine("scan: nothing found; if this %s adapter needs firmware, BlueV cannot upload it yet",
                firmwareFamilyName(m_family));
    }
    return static_cast<int>(m_devices.size());
}

} // namespace bluev
