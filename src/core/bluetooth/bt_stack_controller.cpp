// SPDX-License-Identifier: GPL-3.0-or-later
// controller bring-up and shutdown
#include <cstring>

#include "core/bluetooth/bt_stack.hpp"
#include "core/bluetooth/firmware.hpp"
#include "core/bluetooth/hci_defs.hpp"
#include "core/bluetooth/names.hpp"
#include "core/bytes.hpp"
#include "core/ports/log.hpp"

namespace bluev
{

namespace
{

// Set Event Mask: all events enabled.
const std::uint8_t kEventMask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
const std::uint8_t kSimplePairingOn = 0x01;
// Inquiry results that carry the signal strength and the extended inquiry data (names).
const std::uint8_t kInquiryModeExtended = 0x02;
// Class of device 0x000104, least significant byte first: a desktop computer.
const std::uint8_t kClassOfDevice[3] = {0x04, 0x01, 0x00};

} // namespace

void BtStack::dropStaleLinks()
{
    if (m_external)
    {
        setScan(0);
    }

    if (m_handle != 0 && !m_disconnected)
    {
        sendDisconnect(m_handle);
        wait([this] { return m_disconnected; }, 1500, false);
    }

    // the controller can hold links from a killed payload
    dropAllLinks(40);
    poll(200);

    m_disconnected = false;
    clearConnectResult();
    m_handle = 0;
    logLine("radio: leftover links cleared");
}

void BtStack::useAdapter(const std::string& node)
{
    HciLayout layout;
    const char* which = node.empty() ? m_usb.internalNode() : node.c_str();

    m_node = node;
    m_external = !node.empty();
    m_family = FirmwareFamily::Unknown;
    m_layoutKnown = false;
    m_layout = HciLayout{};
    // the same lookup for the console's chip and for an adapter: which
    // endpoint is which comes from the descriptors, not from a guess
    if (m_usb.discoverLayout(which, layout))
    {
        logLine("%s %s: Bluetooth interface %u, events %#x, ACL in %#x, ACL "
                "out %#x (from its descriptors)",
                m_external ? "adapter" : "console chip", which, layout.interface, layout.events, layout.aclIn,
                layout.aclOut);
        m_layout = layout;
        m_layoutKnown = true;
    }
    else if (m_external)
    {
        logLine("adapter %s: descriptors not available, assuming the standard "
                "layout (0x81, 0x82, 0x02)",
                which);
        m_layoutKnown = true;
    }
    else if (m_internalStandard)
    {
        m_layoutKnown = true;
    }
    else
    {
        logLine("console chip: descriptors not available, detecting the "
                "layout by behaviour");
        m_layout.events = 0x82; // detected in start()
        m_layout.aclIn = 0x81;
        m_layout.aclOut = 0x01;
    }
    m_transport.select(node.empty() ? nullptr : node.c_str(), m_layout);
}

// Opens the transport. The console's controller exposes the event and ACL
// pipes on different endpoints depending on the hardware revision; when they
// are not known from the descriptors, find out where the events come from and
// reopen with the right roles.
bool BtStack::openTransport()
{
    if (!m_transport.open())
    {
        m_startError =
            m_external ? "the adapter could not be opened" : "the console's Bluetooth chip could not be opened";
        return false;
    }
    if (!m_layoutKnown && m_transport.detectRoles(10000) == 2)
    {
        logLine("hci: events arrive on the second IN pipe, "
                "switching to the standard endpoint layout");
        m_transport.close();
        m_layout.events = 0x81;
        m_layout.aclIn = 0x82;
        m_layout.aclOut = 0x02;
        m_transport.select(m_node.empty() ? nullptr : m_node.c_str(), m_layout);
        m_internalStandard = true;
        if (!m_transport.open())
        {
            return false;
        }
    }
    return true;
}

// Some adapters (the Realtek USB family, for one) boot into a ROM that answers
// HCI but cannot scan, and need a patch uploaded at every power-up. The patch
// is not part of BlueV: it is read from the firmware directory. Families whose
// ROM is usable (CSR, most Broadcom sticks) are left alone.
bool BtStack::bringUpAdapterFirmware()
{
    if (!hciSync(hci::kReadLocalVersion))
    {
        logLine("adapter: local version not answered, no firmware step");
        return true;
    }
    ChipIdentity identity;

    if (!chipIdentityFromLocalVersion(m_cc, m_ccLen, identity))
    {
        return true;
    }
    m_family = firmwareFamily(identity);

    FirmwareHci hci{[this](std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length, int attempts) {
                        return hciSync(opcode, params, length, attempts);
                    },
                    [this](std::uint32_t& length) -> const std::uint8_t* {
                        length = m_ccLen;
                        return m_cc;
                    },
                    [this](std::uint16_t opcode, const std::uint8_t* params, std::uint8_t length) {
                        return cmd(opcode, params, length);
                    },
                    [this](int milliseconds) { poll(static_cast<std::uint32_t>(milliseconds)); }};
    const FirmwareResult result = installFirmware(identity, m_firmware, hci);

    if (!result.ok)
    {
        m_startError = result.message;
        m_transport.close();
        return false;
    }
    if (!result.message.empty())
    {
        logLine("adapter: %s", result.message.c_str());
    }
    return true;
}

// the number and size of the controller's ACL buffers, with a safe default when
// it does not say
void BtStack::readBufferSize()
{
    if (hciSync(hci::kReadBufferSize))
    {
        m_credits = getLe16(m_cc + 9);
        m_aclMtu = getLe16(m_cc + 6);
    }
    else
    {
        logLine("controller: buffer size reply missed, using 7 x 1021");
        m_credits = 0;
        m_aclMtu = 0;
    }
    if (m_aclMtu < 0x40 || m_credits < 1)
    {
        m_aclMtu = 0x3fd;
        m_credits = 7;
    }
    m_creditsMax = m_credits;
    logLine("controller: %d ACL buffers of %u bytes", m_credits, m_aclMtu);
}

// Who the controller is: version, features, address. False (with the error
// set and the transport closed) for an adapter that cannot do Classic audio.
bool BtStack::identifyController()
{
    if (hciSync(hci::kReadLocalVersion) && m_ccLen >= 14)
    {
        const unsigned hv = m_cc[6], lmp = m_cc[9];
        const unsigned man = getLe16(m_cc + 10);

        logLine("controller: HCI %s (%#x), LMP %#x, manufacturer %#06x (%s), "
                "revision %#x/%#x",
                hciVersionName(hv), hv, lmp, man, manufacturerName(man), getLe16(m_cc + 7), getLe16(m_cc + 12));
    }
    else
    {
        logLine("controller: local version not answered");
    }
    // byte 4 bit 5 of the features: BR/EDR not supported (a Low Energy only adapter)
    if (hciSync(hci::kReadLocalFeatures) && m_ccLen >= 14 && (m_cc[6 + 4] & 0x20))
    {
        logLine("controller: no BR/EDR support (features %02x %02x %02x "
                "%02x %02x)",
                m_cc[6], m_cc[7], m_cc[8], m_cc[9], m_cc[10]);
        if (m_external)
        {
            m_startError = "this adapter has no Bluetooth Classic (BR/EDR); audio needs it";
            m_transport.close();
            return false;
        }
    }
    if (hciSync(hci::kReadBdAddr) && m_ccLen >= 12)
    {
        std::memcpy(m_local.data(), m_cc + 6, 6);
        m_localKnown = true;
        logLine("controller address %s", addressText(m_local).c_str());
    }
    else
    {
        m_local = BdAddr{};
        m_localKnown = true; // pairings go under a zero address
        logLine("controller address unknown");
    }
    return true;
}

// settings the controller already has, for the log
void BtStack::logControllerSettings()
{
    if (hciSync(hci::kReadPageTimeout) && m_ccLen >= 8)
    {
        logLine("controller: page timeout %u ms", static_cast<unsigned>(getLe16(m_cc + 6) * 625 / 1000));
    }
    if (hciSync(hci::kReadScanEnable) && m_ccLen >= 7)
    {
        logLine("controller: scan setting at start %u (%s)", m_cc[6],
                m_cc[6] == 0   ? "off"
                : m_cc[6] == 1 ? "inquiry scan"
                : m_cc[6] == 2 ? "page scan"
                               : "inquiry and page scan");
        m_systemScan = m_cc[6];
    }
}

// what we need from the controller: all events, simple pairing, extended
// inquiry results, an audio device class, our name, and no scanning yet. The
// console's chip keeps its class, name and scan setting: they are its stack's.
void BtStack::configureController()
{
    std::uint8_t name[248];
    const std::uint8_t scanOff = 0;

    hciSync(hci::kSetEventMask, kEventMask, 8);
    hciSync(hci::kWriteSimplePairing, &kSimplePairingOn, 1);
    hciSync(hci::kWriteInquiryMode, &kInquiryModeExtended, 1);
    if (!m_external)
    {
        return;
    }
    hciSync(hci::kWriteClassOfDevice, kClassOfDevice, 3);

    std::memset(name, 0, sizeof name);
    std::strcpy(reinterpret_cast<char*>(name), "BlueV");
    hciSync(hci::kWriteLocalName, name, 248);
    hciSync(hci::kWriteScanEnable, &scanOff, 1);
}

bool BtStack::start()
{
    m_startError.clear();
    m_systemScan = 0;
    if (!openTransport())
    {
        return false;
    }
    // An adapter that is ours alone starts from a clean state; the console's
    // chip is never reset, the console's stack is using it.
    if (m_external && !hciSync(hci::kReset))
    {
        m_startError = "the adapter did not answer (HCI Reset)";
        m_transport.close();
        return false;
    }
    if (m_external && !bringUpAdapterFirmware())
    {
        return false;
    }
    readBufferSize();
    if (!identifyController())
    {
        return false;
    }
    logControllerSettings();
    configureController();
    dropStaleLinks();
    restoreScan(); // page scan (so we stay findable)
    return true;
}

void BtStack::stop()
{
    dropStaleLinks();
    // the system needs page scan on so that paired controllers can reconnect
    // to the console
    restoreScan();
    m_transport.close();
    m_localKnown = false;
}

// Page scan on an adapter. The console's chip gets back the setting its stack
// had; page scan found off is what a killed copy leaves, so it is turned on.
void BtStack::restoreScan()
{
    setScan(!m_external && m_systemScan >= 2 ? m_systemScan : 2);
}

void BtStack::setScan(int mode)
{
    const std::uint8_t scan = static_cast<std::uint8_t>(mode);

    cmd(hci::kWriteScanEnable, &scan, 1);
    poll(50);
}

} // namespace bluev
