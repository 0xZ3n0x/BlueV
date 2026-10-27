// SPDX-License-Identifier: GPL-3.0-or-later
// Controller families, by the manufacturer Read Local Version reports.
#include "core/bluetooth/firmware.hpp"

#include <cstdio>

#include "core/bluetooth/realtek_firmware.hpp"
#include "core/bytes.hpp"
#include "core/ports/firmware_store.hpp"

namespace bluev
{

namespace
{

constexpr std::uint16_t kManufacturerIntel = 0x0002;
constexpr std::uint16_t kManufacturerCsr = 0x000a;
constexpr std::uint16_t kManufacturerBroadcom = 0x000f;
constexpr std::uint16_t kManufacturerMediaTek = 0x0046;
constexpr std::uint16_t kManufacturerRealtek = 0x005d;

} // namespace

bool chipIdentityFromLocalVersion(const std::uint8_t* event, std::uint32_t length, ChipIdentity& out)
{
    // 0e len ncmd op_lo op_hi status hci_ver hci_rev(2) lmp_ver maker(2) lmp_sub(2)
    if (event == nullptr || length < 14 || event[0] != 0x0e || event[5] != 0 || getLe16(event + 3) != 0x1001)
    {
        return false;
    }
    out = ChipIdentity{};
    out.hciVersion = event[6];
    out.hciRevision = getLe16(event + 7);
    out.lmpVersion = event[9];
    out.manufacturer = getLe16(event + 10);
    out.lmpSubversion = getLe16(event + 12);
    return true;
}

FirmwareFamily firmwareFamily(const ChipIdentity& identity)
{
    switch (identity.manufacturer)
    {
    case kManufacturerCsr:
        return FirmwareFamily::Csr;
    case kManufacturerRealtek:
        return FirmwareFamily::Realtek;
    case kManufacturerBroadcom:
        return FirmwareFamily::Broadcom;
    case kManufacturerIntel:
        return FirmwareFamily::Intel;
    case kManufacturerMediaTek:
        return FirmwareFamily::MediaTek;
    default:
        return FirmwareFamily::Unknown;
    }
}

const char* firmwareFamilyName(FirmwareFamily family)
{
    switch (family)
    {
    case FirmwareFamily::Csr:
        return "CSR";
    case FirmwareFamily::Realtek:
        return "Realtek";
    case FirmwareFamily::Broadcom:
        return "Broadcom";
    case FirmwareFamily::Intel:
        return "Intel";
    case FirmwareFamily::MediaTek:
        return "MediaTek";
    case FirmwareFamily::Unknown:
        break;
    }
    return "unknown";
}

bool firmwareUploadSupported(FirmwareFamily family)
{
    return family == FirmwareFamily::Realtek;
}

FirmwareResult installFirmware(const ChipIdentity& identity, IFirmwareStore& files, FirmwareHci& hci)
{
    const FirmwareFamily family = firmwareFamily(identity);

    switch (family)
    {
    case FirmwareFamily::Realtek: {
        const RealtekChip* chip = realtekChipFor(identity);

        if (chip == nullptr)
        {
            if (!realtekInRomMode(identity))
            {
                return {true, ""}; // already patched, or a chip without a ROM-mode patch
            }
            char text[96];

            std::snprintf(text, sizeof text,
                          "Realtek %04x/%04x/%02x is not a chip with a known patch, starting as it is",
                          identity.lmpSubversion, identity.hciRevision, identity.hciVersion);
            return {true, text};
        }
        return realtekInstall(*chip, files, hci);
    }
    case FirmwareFamily::Csr:
    case FirmwareFamily::Unknown:
        return {true, ""};
    default:
        return {true, std::string(firmwareFamilyName(family)) + " firmware is not supported yet, starting as it is"};
    }
}

} // namespace bluev
