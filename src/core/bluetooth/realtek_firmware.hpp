// SPDX-License-Identifier: GPL-3.0-or-later
// Realtek USB controllers that boot from ROM. They answer HCI and appear in
// the adapter list, but their radio cannot scan until the host uploads the
// patch: the same job Linux's btrtl does, over the vendor commands 0xfc6d
// (ROM version) and 0xfc20 (the patch, in 252 byte fragments).
//
// Both blob formats are accepted: the "Realtech" v1 file from linux-firmware
// and the "RTBTCore" v2 file from the Windows 6.0 driver. They differ only in
// the container; the upload is the same.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/bluetooth/firmware.hpp"
#include "core/ports/firmware_store.hpp"

namespace bluev
{

// One chip as it identifies itself in ROM mode. After the upload the revision
// changes, so a match means "this chip still needs the patch".
struct RealtekChip
{
    std::uint16_t lmpSubversion = 0;
    std::uint16_t hciRevision = 0;
    std::uint8_t hciVersion = 0;
    const char* name = "";         // "RTL8761BU"
    const char* firmwareFile = ""; // "rtl8761bu_fw.bin"
    const char* configFile = nullptr;
    bool configRequired = false;
};

// Null when the identity is not a known Realtek USB chip in ROM mode.
const RealtekChip* realtekChipFor(const ChipIdentity& identity);
// True when the LMP subversion is one of the ROM values: the chip still needs
// a patch, even when its exact revision is not in the table. After an upload the
// subversion changes, so this is false for a chip that already runs firmware.
bool realtekInRomMode(const ChipIdentity& identity);

// Reads the ROM version, loads the files and uploads the patch. On success
// `message` is a one-line summary; on failure it says what to do.
FirmwareResult realtekInstall(const RealtekChip& chip, IFirmwareStore& files, FirmwareHci& hci);

// The pure parts, exposed for the tests.
bool realtekImage(const std::vector<std::uint8_t>& firmware, const std::vector<std::uint8_t>& config,
                  std::uint8_t romVersion, std::vector<std::uint8_t>& out, std::string& why);
std::vector<std::vector<std::uint8_t>> realtekFragments(const std::vector<std::uint8_t>& image);

} // namespace bluev
