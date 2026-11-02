// SPDX-License-Identifier: GPL-3.0-or-later
// Realtek USB controllers that boot from ROM. They answer HCI and appear in
// the adapter list, but their radio cannot scan until the host uploads the
// patch: the same job Linux's btrtl does, over the vendor commands 0xfc6d
// (ROM version), 0xfc61 (the security key id) and 0xfc20 (the patch, in 252
// byte fragments).
//
// Both blob formats are accepted: the "Realtech" v1 file from linux-firmware
// and the "RTBTCore" v2 file from the Windows 6.0 driver. They differ only in
// the container; the upload is the same. Chips with a non-zero key id carry
// their patch in v2 security sections instead of the plain snippets.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/bluetooth/firmware.hpp"
#include "core/ports/firmware_store.hpp"

namespace bluev
{

// One chip as it identifies itself in ROM mode. After the upload the revision
// changes, so a match means "this chip still needs the patch". An hciVersion of
// 0 is a wildcard, as in btrtl's table (the RTL8761CU row uses it).
struct RealtekChip
{
    std::uint16_t lmpSubversion = 0;
    std::uint16_t hciRevision = 0;
    std::uint8_t hciVersion = 0;
    const char* name = "";         // "RTL8761BU"
    const char* firmwareFile = ""; // "rtl8761bu_fw.bin"
    const char* configFile = nullptr;
    bool configRequired = false;
    // Tried before firmwareFile when set: the RTL8852C also ships a _v2 file.
    const char* firmwareFileV2 = nullptr;
};

// What the container parser needs to know about the chip.
struct RealtekTarget
{
    std::uint16_t lmpSubversion = 0; // the file's project id must be for this chip
    std::uint8_t romVersion = 0;     // selects the patch (eco = romVersion + 1)
    std::uint8_t keyId = 0;          // 0: no security sections, the config file is used
};

// Null when the identity is not a known Realtek USB chip in ROM mode.
const RealtekChip* realtekChipFor(const ChipIdentity& identity);
// True when the LMP subversion is one of the ROM values: the chip still needs
// a patch, even when its exact revision is not in the table. After an upload the
// subversion changes, so this is false for a chip that already runs firmware.
bool realtekInRomMode(const ChipIdentity& identity);

// Identifies the chip (dropping a vendor patch with 0xfc66 first, as btrtl
// does), reads the ROM version and key id, loads the files and uploads the
// patch. On success `message` is a one-line summary; on failure it says what to
// do.
FirmwareResult realtekInstall(const ChipIdentity& identity, IFirmwareStore& files, FirmwareHci& hci);

// The pure parts, exposed for the tests.
bool realtekImage(const std::vector<std::uint8_t>& firmware, const std::vector<std::uint8_t>& config,
                  const RealtekTarget& target, std::vector<std::uint8_t>& out, std::string& why);
std::vector<std::vector<std::uint8_t>> realtekFragments(const std::vector<std::uint8_t>& image);

} // namespace bluev
