// SPDX-License-Identifier: GPL-3.0-or-later
// The chip table, the container layouts, the project ids and the fragment
// rules follow drivers/bluetooth/btrtl.c in Linux (GPL-2.0-or-later, see its
// copyright note there), adapted to this stack. The firmware files themselves
// are not part of BlueV: the user puts them on the console (see README.md).
#include "core/bluetooth/realtek_firmware.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include "core/bluetooth/hci_defs.hpp"
#include "core/bytes.hpp"

namespace bluev
{

namespace
{

constexpr std::uint16_t kReadRomVersion = 0xfc6d;
constexpr std::uint16_t kDownloadPatch = 0xfc20;
constexpr std::uint16_t kReadReg16 = 0xfc61;
constexpr std::uint16_t kDropFirmware = 0xfc66;
constexpr std::uint16_t kReset = 0x0c03;

// The register behind the key id (RTL_SEC_PROJ), read with 0xfc61.
constexpr std::uint8_t kSecProj[5] = {0x10, 0xa4, 0xad, 0x00, 0xb0};

constexpr std::size_t kFragment = 252;
constexpr std::size_t kEpatchHeader = 14;   // signature(8) + version(4) + patches(2)
constexpr std::size_t kEpatchHeaderV2 = 20; // signature(8) + version(8) + sections(4)
constexpr std::size_t kExtension = 7;       // the version instruction (3) and the signature (4)
constexpr std::uint8_t kExtensionSignature[4] = {0x51, 0x04, 0xfd, 0x77};

// v2 section opcodes, from btrtl
constexpr std::uint32_t kSectionSnippets = 1;
constexpr std::uint32_t kSectionDummy = 2;
constexpr std::uint32_t kSectionSecurity = 3;

// The USB chips btrtl knows, with the firmware file for each. The UART variants
// and the 8723A (which uses a different, older download) are left out.
constexpr RealtekChip kChips[] = {
    {0x8723, 0x000b, 0x06, "RTL8723BU", "rtl8723b_fw.bin", "rtl8723b_config.bin", false},
    {0x8723, 0x000d, 0x08, "RTL8723DU", "rtl8723d_fw.bin", "rtl8723d_config.bin", true},
    {0x8821, 0x000a, 0x06, "RTL8821AU", "rtl8821a_fw.bin", "rtl8821a_config.bin", false},
    {0x8821, 0x000c, 0x08, "RTL8821CU", "rtl8821c_fw.bin", "rtl8821c_config.bin", false},
    {0x8761, 0x000a, 0x06, "RTL8761AU", "rtl8761a_fw.bin", "rtl8761a_config.bin", false},
    {0x8761, 0x000b, 0x0a, "RTL8761BU", "rtl8761bu_fw.bin", "rtl8761bu_config.bin", false},
    // btrtl leaves the HCI version of this row open (a wildcard version)
    {0x8761, 0x000e, 0x00, "RTL8761CU", "rtl8761cu_fw.bin", "rtl8761cu_config.bin", false},
    {0x8822, 0x000b, 0x07, "RTL8822BU", "rtl8822b_fw.bin", "rtl8822b_config.bin", true},
    {0x8822, 0x000c, 0x0a, "RTL8822CU", "rtl8822cu_fw.bin", "rtl8822cu_config.bin", false},
    {0x8852, 0x000a, 0x0b, "RTL8852AU", "rtl8852au_fw.bin", "rtl8852au_config.bin", false},
    {0x8852, 0x000b, 0x0b, "RTL8852BU", "rtl8852bu_fw.bin", "rtl8852bu_config.bin", false},
    {0x8852, 0x000c, 0x0c, "RTL8852CU", "rtl8852cu_fw.bin", "rtl8852cu_config.bin", false, "rtl8852cu_fw_v2.bin"},
    {0x8851, 0x000b, 0x0c, "RTL8851BU", "rtl8851bu_fw.bin", "rtl8851bu_config.bin", false},
    {0x8922, 0x000a, 0x0c, "RTL8922AU", "rtl8922au_fw.bin", "rtl8922au_config.bin", false},
    {0x8852, 0x0087, 0x0c, "RTL8852BTU", "rtl8852btu_fw.bin", "rtl8852btu_config.bin", false},
};

// What the version instruction in the file's extension names: the project id
// of a chip family. btrtl compares the resulting LMP subversion with the chip's.
struct ProjectId
{
    std::uint8_t id;
    std::uint16_t lmpSubversion;
};

constexpr ProjectId kProjects[] = {
    {0, 0x8723},  {1, 0x8723},  {2, 0x8821},  {3, 0x8761},  {7, 0x8703},  {8, 0x8822},
    {9, 0x8723},  {10, 0x8821}, {13, 0x8822}, {14, 0x8761}, {18, 0x8852}, {20, 0x8852},
    {25, 0x8852}, {36, 0x8851}, {44, 0x8922}, {47, 0x8852}, {51, 0x8761},
};

bool hasSignature(const std::vector<std::uint8_t>& data, const char* signature)
{
    return data.size() >= 8 && std::memcmp(data.data(), signature, 8) == 0;
}

// btrtl walks backwards from the extension signature to find the instruction
// that names the project id (opcode 0, length 1, the id is its data byte).
// Returns -1 when no such instruction is there.
int projectIdOf(const std::vector<std::uint8_t>& firmware, std::size_t floor)
{
    const std::uint8_t* data = firmware.data();
    std::size_t at = firmware.size() - sizeof kExtensionSignature;

    while (at >= floor + 3)
    {
        const std::uint8_t opcode = data[--at];
        const std::uint8_t length = data[--at];
        const std::uint8_t value = data[--at];

        if (opcode == 0xff) // end of the instructions
        {
            break;
        }
        if (length == 0 || length > at)
        {
            return -1;
        }
        if (opcode == 0 && length == 1)
        {
            return value;
        }
        at -= length;
    }
    return -1;
}

// The LMP subversion a project id belongs to, or -1 when it is not a known one.
int lmpSubversionForProject(unsigned project)
{
    for (const ProjectId& known : kProjects)
    {
        if (known.id == project)
        {
            return known.lmpSubversion;
        }
    }
    return -1;
}

// v1: header, then two arrays per patch (chip id, length, offset), then the
// patches. The patch for this chip is copied and stamped with the file's
// version in its last four bytes.
bool buildV1(const std::vector<std::uint8_t>& firmware, std::uint8_t romVersion, std::vector<std::uint8_t>& out,
             std::string& why)
{
    const std::uint16_t patches = getLe16(firmware.data() + 12);

    if (patches == 0 || patches > 256)
    {
        why = "it has no patch table";
        return false;
    }
    const std::size_t table = kEpatchHeader + 8u * patches;

    if (firmware.size() < table)
    {
        why = "the patch table is cut off";
        return false;
    }

    const std::uint8_t* ids = firmware.data() + kEpatchHeader;
    const std::uint8_t* lengths = ids + 2u * patches;
    const std::uint8_t* offsets = lengths + 2u * patches;
    const std::uint16_t wanted = static_cast<std::uint16_t>(romVersion + 1);

    for (std::uint16_t i = 0; i < patches; i++)
    {
        if (getLe16(ids + 2u * i) != wanted)
        {
            continue;
        }
        const std::uint16_t length = getLe16(lengths + 2u * i);
        const std::uint32_t offset = getLe32(offsets + 4u * i);

        if (offset == 0 || length < 4 || offset > firmware.size() || length > firmware.size() - offset)
        {
            why = "its patch for this chip is out of range";
            return false;
        }
        out.assign(firmware.begin() + offset, firmware.begin() + offset + length);
        std::memcpy(out.data() + out.size() - 4, firmware.data() + 8, 4); // the file's version
        return true;
    }
    why = "it has no patch for ROM version " + std::to_string(romVersion);
    return false;
}

struct Piece
{
    std::uint8_t priority;
    std::vector<std::uint8_t> data;
};

// btrtl inserts before the first subsection whose priority is >= the new one,
// so equal priorities end up in reverse file order. Keep that order to build
// the same patch as Linux does.
void insertInPriorityOrder(std::vector<Piece>& pieces, Piece piece)
{
    auto at = pieces.begin();

    while (at != pieces.end() && at->priority < piece.priority)
    {
        ++at;
    }
    pieces.insert(at, std::move(piece));
}

// v2: a section list. Each section carries subsections for the chip's ROM
// version (the "eco"); they are applied in priority order. Security sections
// are only used by chips with a key id, and then only the subsections for that
// key id.
bool buildV2(const std::vector<std::uint8_t>& firmware, std::uint8_t romVersion, std::uint8_t keyId,
             std::vector<std::uint8_t>& out, std::string& why)
{
    if (firmware.size() < kEpatchHeaderV2 + kExtension)
    {
        why = "the file is too short";
        return false;
    }
    const std::size_t end = firmware.size() - kExtension;
    const std::uint32_t sections = getLe32(firmware.data() + 16);
    std::size_t pos = kEpatchHeaderV2;
    std::vector<Piece> pieces;

    for (std::uint32_t s = 0; s < sections; s++)
    {
        if (pos + 8 > end)
        {
            why = "a section is cut off";
            return false;
        }
        const std::uint32_t opcode = getLe32(firmware.data() + pos);
        const std::uint32_t length = getLe32(firmware.data() + pos + 4);

        pos += 8;
        if (length > end - pos)
        {
            why = "a section is cut off";
            return false;
        }
        const std::size_t sectionEnd = pos + length;

        switch (opcode)
        {
        case kSectionSnippets:
        case kSectionDummy:
        case kSectionSecurity: {
            if (opcode == kSectionSecurity && keyId == 0)
            {
                break; // ignored for a chip without a key id, as btrtl does
            }
            if (length < 4)
            {
                why = "a section is cut off";
                return false;
            }
            const std::uint16_t count = getLe16(firmware.data() + pos);
            std::size_t at = pos + 4;

            for (std::uint16_t i = 0; i < count; i++)
            {
                if (at + 8 > sectionEnd)
                {
                    why = "a subsection is cut off";
                    return false;
                }
                const std::uint8_t eco = firmware[at];
                const std::uint8_t priority = firmware[at + 1];
                const std::uint8_t sectionKey = firmware[at + 2];
                const std::uint32_t size = getLe32(firmware.data() + at + 4);

                at += 8;
                if (size > sectionEnd - at)
                {
                    why = "a subsection is cut off";
                    return false;
                }
                if (eco == static_cast<std::uint8_t>(romVersion + 1) &&
                    (opcode != kSectionSecurity || sectionKey == keyId))
                {
                    insertInPriorityOrder(pieces, {priority, std::vector<std::uint8_t>(firmware.begin() + at,
                                                                                       firmware.begin() + at + size)});
                }
                at += size;
            }
            break;
        }
        default:
            break; // a section type this loader does not know
        }
        pos = sectionEnd;
    }
    if (pieces.empty())
    {
        why = "it has no patch for ROM version " + std::to_string(romVersion);
        return false;
    }
    out.clear();
    for (const Piece& piece : pieces)
    {
        out.insert(out.end(), piece.data.begin(), piece.data.end());
    }
    return true;
}

std::string firmwareNames(const RealtekChip& chip)
{
    if (chip.firmwareFileV2 != nullptr)
    {
        return std::string(chip.firmwareFileV2) + " or " + chip.firmwareFile;
    }
    return chip.firmwareFile;
}

std::string missingFirmware(const RealtekChip& chip, IFirmwareStore& files, bool withConfig)
{
    std::string what = firmwareNames(chip);

    if (withConfig)
    {
        what += " and ";
        what += chip.configFile;
    }
    return "this adapter needs firmware: put " + what + " in " + files.location() + "/";
}

bool readLocalVersion(FirmwareHci& hci, ChipIdentity& out)
{
    if (!hci.sync(hci::kReadLocalVersion, nullptr, 0, 2))
    {
        return false;
    }
    std::uint32_t length = 0;
    const std::uint8_t* reply = hci.reply(length);

    return chipIdentityFromLocalVersion(reply, length, out);
}

} // namespace

const RealtekChip* realtekChipFor(const ChipIdentity& identity)
{
    for (const RealtekChip& chip : kChips)
    {
        if (chip.lmpSubversion == identity.lmpSubversion && chip.hciRevision == identity.hciRevision &&
            (chip.hciVersion == 0 || chip.hciVersion == identity.hciVersion))
        {
            return &chip;
        }
    }
    return nullptr;
}

bool realtekInRomMode(const ChipIdentity& identity)
{
    for (const RealtekChip& chip : kChips)
    {
        if (chip.lmpSubversion == identity.lmpSubversion)
        {
            return true;
        }
    }
    return false;
}

bool realtekImage(const std::vector<std::uint8_t>& firmware, const std::vector<std::uint8_t>& config,
                  const RealtekTarget& target, std::vector<std::uint8_t>& out, std::string& why)
{
    out.clear();
    why.clear();
    const bool v1 = hasSignature(firmware, "Realtech");
    const bool v2 = !v1 && hasSignature(firmware, "RTBTCore");

    if (!v1 && !v2)
    {
        why = "it is not a Realtek firmware file";
        return false;
    }
    const std::size_t header = v1 ? kEpatchHeader : kEpatchHeaderV2;

    if (firmware.size() < header + kExtension)
    {
        why = "the file is too short";
        return false;
    }
    if (std::memcmp(firmware.data() + firmware.size() - sizeof kExtensionSignature, kExtensionSignature,
                    sizeof kExtensionSignature) != 0)
    {
        why = "its extension section is missing or damaged";
        return false;
    }
    const int project = projectIdOf(firmware, header);

    if (project < 0)
    {
        why = "it has no project id";
        return false;
    }
    const int expected = lmpSubversionForProject(static_cast<unsigned>(project));

    if (expected < 0)
    {
        why = "its project id " + std::to_string(project) + " is not a known Realtek chip";
        return false;
    }
    if (expected != target.lmpSubversion)
    {
        why = "it is for another chip (project id " + std::to_string(project) + ")";
        return false;
    }
    if (v1)
    {
        if (!buildV1(firmware, target.romVersion, out, why))
        {
            return false;
        }
    }
    else if (!buildV2(firmware, target.romVersion, target.keyId, out, why))
    {
        return false;
    }
    // the chip parses the vendor config appended after the patch; a chip with a
    // key id takes its settings from the security sections instead (btrtl skips
    // the config file for those)
    if (target.keyId == 0)
    {
        out.insert(out.end(), config.begin(), config.end());
    }
    return true;
}

std::vector<std::vector<std::uint8_t>> realtekFragments(const std::vector<std::uint8_t>& image)
{
    std::vector<std::vector<std::uint8_t>> fragments;
    const std::size_t count = image.size() / kFragment + 1;
    std::size_t index = 0;

    for (std::size_t i = 0; i < count; i++)
    {
        std::uint8_t header = static_cast<std::uint8_t>(index++);
        std::size_t length = kFragment;

        if (header == 0x7f) // 0x80 is the end marker
        {
            index = 1;
        }
        if (i == count - 1)
        {
            header |= 0x80;
            length = image.size() % kFragment;
        }
        std::vector<std::uint8_t> fragment;
        fragment.reserve(length + 1);
        fragment.push_back(header);
        if (length > 0)
        {
            fragment.insert(fragment.end(), image.begin() + i * kFragment, image.begin() + i * kFragment + length);
        }
        fragments.push_back(std::move(fragment));
    }
    return fragments;
}

FirmwareResult realtekInstall(const ChipIdentity& identity, IFirmwareStore& files, FirmwareHci& hci)
{
    ChipIdentity chipIdentity = identity;
    const RealtekChip* chip = realtekChipFor(chipIdentity);

    // A Realtek that matches no row may already carry a vendor patch, whose
    // numbers are not the ROM ones. btrtl sends 0xfc66 to drop that patch, waits
    // 200 ms and reads the version again.
    if (chip == nullptr && hci.send)
    {
        hci.send(kDropFirmware, nullptr, 0);
        if (hci.pause)
        {
            hci.pause(200);
        }
        ChipIdentity again;
        if (readLocalVersion(hci, again))
        {
            chipIdentity = again;
            chip = realtekChipFor(chipIdentity);
        }
    }
    if (chip == nullptr)
    {
        if (!realtekInRomMode(chipIdentity))
        {
            return {true, ""}; // already patched, or a chip without a ROM-mode patch
        }
        char text[96];

        std::snprintf(text, sizeof text, "Realtek %04x/%04x/%02x is not a chip with a known patch, starting as it is",
                      chipIdentity.lmpSubversion, chipIdentity.hciRevision, chipIdentity.hciVersion);
        return {true, text};
    }

    if (!hci.sync(kReadRomVersion, nullptr, 0, 3))
    {
        return {false, "the adapter did not report its Realtek ROM version"};
    }
    std::uint32_t length = 0;
    const std::uint8_t* reply = hci.reply(length);

    if (reply == nullptr || length < 7 || reply[5] != 0)
    {
        return {false, "the adapter did not report its Realtek ROM version"};
    }
    const std::uint8_t romVersion = reply[6];

    // The key id decides whether the patch lives in the v2 security sections and
    // whether the config file is used at all (btrtl reads RTL_SEC_PROJ always).
    if (!hci.sync(kReadReg16, kSecProj, static_cast<std::uint8_t>(sizeof kSecProj), 3))
    {
        return {false, "the adapter did not report its Realtek key id"};
    }
    reply = hci.reply(length);
    if (reply == nullptr || length < 8 || reply[5] != 0)
    {
        return {false, "the adapter did not report its Realtek key id"};
    }
    const std::uint8_t keyId = reply[6];

    const char* first = chip->firmwareFileV2 != nullptr ? chip->firmwareFileV2 : chip->firmwareFile;
    const char* used = first;
    std::vector<std::uint8_t> firmware;

    if (!files.read(first, firmware))
    {
        used = chip->firmwareFile;
        if (chip->firmwareFileV2 == nullptr || !files.read(used, firmware))
        {
            return {false, missingFirmware(*chip, files, false)};
        }
    }
    std::vector<std::uint8_t> config;

    if (keyId == 0 && chip->configFile != nullptr && !files.read(chip->configFile, config) && chip->configRequired)
    {
        return {false, missingFirmware(*chip, files, true)};
    }

    std::vector<std::uint8_t> image;
    std::string why;

    if (!realtekImage(firmware, config, {chip->lmpSubversion, romVersion, keyId}, image, why))
    {
        return {false, std::string("the ") + chip->name + " firmware " + used + " could not be used: " + why};
    }

    const std::vector<std::vector<std::uint8_t>> fragments = realtekFragments(image);
    for (std::size_t i = 0; i < fragments.size(); i++)
    {
        if (!hci.sync(kDownloadPatch, fragments[i].data(), static_cast<std::uint8_t>(fragments[i].size()), 1))
        {
            return {false, "the adapter stopped accepting the firmware (fragment " + std::to_string(i + 1) + " of " +
                               std::to_string(fragments.size()) +
                               "); unplug it and plug it back in before trying again"};
        }
    }
    if (!hci.sync(kReset, nullptr, 0, 5))
    {
        return {false, "the adapter did not answer the reset after the firmware upload; unplug it and plug it back "
                       "in before trying again"};
    }
    return {true, std::string(chip->name) + " firmware " + used + " uploaded (" + std::to_string(image.size()) +
                      " bytes in " + std::to_string(fragments.size()) + " fragments)"};
}

} // namespace bluev
